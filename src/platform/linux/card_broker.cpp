/**
 * @file src/platform/linux/card_broker.cpp
 * @brief Client for the privileged Hermes-KMS card broker.
 */
#include "card_broker.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/logging.h"
#include "src/utility.h"

namespace VDISPLAY::card_broker {

  namespace {

    constexpr const char *default_socket_path = "/run/hermes/card-broker.sock";
    constexpr size_t max_reply_bytes = 4096;

    // The whole conversation - connect, request, reply - shares one deadline.
    // The broker accepts serially, so a request may have to wait out another
    // client's slow connection first; a broker that cannot answer within this
    // must not hang the display bring-up behind it.
    constexpr auto broker_timeout = std::chrono::seconds(15);

    /**
     * @brief Wait until `fd` is ready for `events` or the deadline passes.
     *
     * True when the descriptor is ready (or hung up, which the read or write
     * that follows reports precisely); false when the deadline passed first.
     */
    bool wait_ready(int fd, short events, std::chrono::steady_clock::time_point deadline) {
      for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          return false;
        }
        pollfd descriptor {fd, events, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready > 0) {
          return (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        if (ready == 0) {
          return false;
        }
        if (errno != EINTR) {
          return false;
        }
      }
    }

    /**
     * @brief connect() with a deadline.
     *
     * A full listen backlog makes connect block indefinitely; with the
     * broker stalled that would freeze the caller in the one place the
     * deadline cannot otherwise reach.
     */
    bool connect_with_timeout(int fd, const sockaddr *address, socklen_t length,
                              std::chrono::steady_clock::time_point deadline) {
      const int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }

      int result = ::connect(fd, address, length);
      if (result != 0 && (errno == EINPROGRESS || errno == EAGAIN)) {
        if (!wait_ready(fd, POLLOUT, deadline)) {
          result = -1;
          errno = ETIMEDOUT;
        } else {
          int socket_error = 0;
          socklen_t error_length = sizeof(socket_error);
          if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
              socket_error != 0) {
            errno = socket_error != 0 ? socket_error : errno;
            result = -1;
          } else {
            result = 0;
          }
        }
      }

      if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags);  // Restore blocking mode.
      }
      return result == 0;
    }

    std::string socket_path() {
      // Overridable so the suite can point at a broker of its own rather than
      // at the machine's.
      if (const char *override_path = std::getenv("HERMES_CARD_BROKER_SOCKET")) {
        return override_path;
      }
      return default_socket_path;
    }

    /**
     * @brief Send one request and return the reply line, or nothing at all.
     *
     * One request per connection: the broker answers and closes, so there is no
     * session to keep alive, nothing to resynchronise after an error, and no
     * way for one caller to hold the socket against another.
     */
    std::optional<std::string> ask(const std::string &request) {
      const auto path = socket_path();
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) {
        BOOST_LOG(error) << "[VDISPLAY/card-broker] Socket path is too long: " << path;
        return std::nullopt;
      }
      std::memcpy(address.sun_path, path.c_str(), path.size());

      const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        BOOST_LOG(error) << "[VDISPLAY/card-broker] Could not create a socket: " << std::strerror(errno);
        return std::nullopt;
      }
      auto guard = util::fail_guard([fd]() {
        ::close(fd);
      });

      const auto deadline = std::chrono::steady_clock::now() + broker_timeout;
      if (!connect_with_timeout(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address), deadline)) {
        BOOST_LOG(debug) << "[VDISPLAY/card-broker] No broker at " << path << ": " << std::strerror(errno);
        return std::nullopt;
      }

      const auto line = request + "\n";
      size_t offset = 0;
      while (offset < line.size()) {
        if (!wait_ready(fd, POLLOUT, deadline)) {
          BOOST_LOG(error) << "[VDISPLAY/card-broker] Timed out sending " << request << " to the broker.";
          return std::nullopt;
        }
        const ssize_t count = ::write(fd, line.data() + offset, line.size() - offset);
        if (count <= 0) {
          BOOST_LOG(error) << "[VDISPLAY/card-broker] Could not send " << request << ": "
                           << std::strerror(errno);
          return std::nullopt;
        }
        offset += static_cast<size_t>(count);
      }
      ::shutdown(fd, SHUT_WR);

      std::string reply;
      std::array<char, 512> buffer {};
      while (reply.size() < max_reply_bytes) {
        if (!wait_ready(fd, POLLIN, deadline)) {
          BOOST_LOG(error) << "[VDISPLAY/card-broker] Timed out waiting for the reply to " << request << '.';
          return std::nullopt;
        }
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
          BOOST_LOG(error) << "[VDISPLAY/card-broker] Could not read the reply to " << request << ": "
                           << std::strerror(errno);
          return std::nullopt;
        }
        if (count == 0) {
          break;
        }
        reply.append(buffer.data(), static_cast<size_t>(count));
        if (reply.find('\n') != std::string::npos) {
          break;
        }
      }

      const auto newline = reply.find('\n');
      if (newline != std::string::npos) {
        reply.resize(newline);
      }
      if (reply.empty()) {
        BOOST_LOG(error) << "[VDISPLAY/card-broker] The broker closed without answering " << request << '.';
        return std::nullopt;
      }
      return reply;
    }

    std::vector<std::string> fields(const std::string &line) {
      std::vector<std::string> out;
      std::istringstream stream {line};
      std::string field;
      while (stream >> field) {
        out.emplace_back(field);
      }
      return out;
    }

    /**
     * @brief The reply's fields when it succeeded, nothing when it did not.
     *        A refusal is logged with the broker's own words, which name the
     *        reason: an unlisted user, a full pool, a card that is not theirs.
     */
    std::optional<std::vector<std::string>> succeeded(const std::optional<std::string> &reply,
                                                      const char *request) {
      if (!reply) {
        return std::nullopt;
      }
      auto parts = fields(*reply);
      if (parts.empty() || parts.front() != "OK") {
        BOOST_LOG(warning) << "[VDISPLAY/card-broker] " << request << " was refused: " << *reply;
        return std::nullopt;
      }
      parts.erase(parts.begin());
      return parts;
    }

  }  // namespace

  bool available() {
    struct stat info {};
    return ::stat(socket_path().c_str(), &info) == 0 && S_ISSOCK(info.st_mode);
  }

  std::optional<card_t> create(std::optional<uid_t> session_account_uid) {
    const auto request = session_account_uid ?
                           "CREATE " + std::to_string(*session_account_uid) :
                           "CREATE";
    const auto parts = succeeded(ask(request), "CREATE");
    if (!parts) {
      return std::nullopt;
    }
    if (parts->size() < 3) {
      BOOST_LOG(error) << "[VDISPLAY/card-broker] CREATE returned no device nodes.";
      return std::nullopt;
    }

    card_t card {(*parts)[0], (*parts)[1], (*parts)[2]};
    BOOST_LOG(info) << "[VDISPLAY/card-broker] Created card " << card.name << " (" << card.card_path
                    << ", " << card.render_node_path << ").";
    return card;
  }

  bool remove(const std::string &name) {
    if (name.empty()) {
      return false;
    }
    return succeeded(ask("REMOVE " + name), "REMOVE").has_value();
  }

  int sweep() {
    const auto parts = succeeded(ask("SWEEP"), "SWEEP");
    if (!parts || parts->empty()) {
      return 0;
    }
    const int removed = std::atoi(parts->front().c_str());
    if (removed > 0) {
      BOOST_LOG(warning) << "[VDISPLAY/card-broker] Removed " << removed
                         << " card(s) left behind by an earlier run.";
    }
    return removed;
  }

}  // namespace VDISPLAY::card_broker
