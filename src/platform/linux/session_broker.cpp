/**
 * @file src/platform/linux/session_broker.cpp
 * @brief Client for the privileged Hermes session account broker.
 */
#include "session_broker.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/logging.h"
#include "src/utility.h"

namespace platf::session_broker {

  namespace {

    constexpr const char *default_socket_path = "/run/hermes/session-broker.sock";
    constexpr size_t max_reply_bytes = 4096;

    std::string socket_path() {
      // Overridable so the suite can point at a broker of its own rather than
      // at the machine's.
      if (const char *override_path = std::getenv("HERMES_SESSION_BROKER_SOCKET")) {
        return override_path;
      }
      return default_socket_path;
    }

    /**
     * @brief Send one request and return the reply line, or nothing at all.
     *
     * One request per connection: the broker answers and closes, so there is no
     * session to keep alive, nothing to resynchronise after an error, and no
     * way for one caller to hold the socket against another. `request` is sent
     * whole, which is what lets START carry a command and an environment
     * without either being split on whitespace.
     */
    std::optional<std::string> ask(const std::string &request) {
      const auto path = socket_path();
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) {
        BOOST_LOG(error) << "[session-broker] Socket path is too long: " << path;
        return std::nullopt;
      }
      std::memcpy(address.sun_path, path.c_str(), path.size());

      const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        BOOST_LOG(error) << "[session-broker] Could not create a socket: " << std::strerror(errno);
        return std::nullopt;
      }
      auto guard = util::fail_guard([fd]() {
        ::close(fd);
      });

      if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        BOOST_LOG(debug) << "[session-broker] No broker at " << path << ": " << std::strerror(errno);
        return std::nullopt;
      }

      // Partial writes are the normal case once a request carries a desktop's
      // worth of environment, so this loops where a single-line protocol could
      // get away with one write.
      size_t offset = 0;
      while (offset < request.size()) {
        const ssize_t count = ::write(fd, request.data() + offset, request.size() - offset);
        if (count <= 0) {
          BOOST_LOG(error) << "[session-broker] Could not send the request: " << std::strerror(errno);
          return std::nullopt;
        }
        offset += static_cast<size_t>(count);
      }
      ::shutdown(fd, SHUT_WR);

      std::string reply;
      std::array<char, 512> buffer {};
      while (reply.size() < max_reply_bytes) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
          BOOST_LOG(error) << "[session-broker] Could not read the reply: " << std::strerror(errno);
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

      if (const auto newline = reply.find('\n'); newline != std::string::npos) {
        reply.resize(newline);
      }
      if (reply.empty()) {
        BOOST_LOG(error) << "[session-broker] The broker closed without answering.";
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
     *        reason: an unlisted user, a full pool, a damaged account map.
     */
    std::optional<std::vector<std::string>> succeeded(
      const std::optional<std::string> &reply,
      const char *request
    ) {
      if (!reply) {
        return std::nullopt;
      }
      auto parts = fields(*reply);
      if (parts.empty() || parts.front() != "OK") {
        BOOST_LOG(warning) << "[session-broker] " << request << " was refused: " << *reply;
        return std::nullopt;
      }
      parts.erase(parts.begin());
      return parts;
    }

    /**
     * @brief Parse the `<user> <uid> <home>` an ENSURE or LOOKUP answers with.
     */
    std::optional<account_t> account_from(
      const std::optional<std::string> &reply,
      const char *request
    ) {
      const auto parts = succeeded(reply, request);
      if (!parts) {
        return std::nullopt;
      }
      if (parts->size() < 3) {
        BOOST_LOG(error) << "[session-broker] " << request << " returned an incomplete account.";
        return std::nullopt;
      }

      account_t account;
      account.user = (*parts)[0];
      account.home = (*parts)[2];
      // A uid of 0 would mean the session runs as root, so a value that does
      // not parse is refused rather than defaulted.
      const auto uid = std::strtoul((*parts)[1].c_str(), nullptr, 10);
      if (uid == 0 || uid > 0xFFFFFFFFUL) {
        BOOST_LOG(error) << "[session-broker] " << request << " returned an unusable uid: "
                         << (*parts)[1];
        return std::nullopt;
      }
      account.uid = static_cast<uid_t>(uid);
      return account;
    }

  }  // namespace

  bool available() {
    struct stat info {};
    return ::stat(socket_path().c_str(), &info) == 0 && S_ISSOCK(info.st_mode);
  }

  std::optional<account_t> ensure(const std::string &client_uuid) {
    if (client_uuid.empty()) {
      return std::nullopt;
    }
    const auto account = account_from(ask("ENSURE " + client_uuid + "\n"), "ENSURE");
    if (account) {
      BOOST_LOG(info) << "[session-broker] Client " << client_uuid << " owns account "
                      << account->user << " (uid " << account->uid << ").";
    }
    return account;
  }

  std::optional<account_t> lookup(const std::string &client_uuid) {
    if (client_uuid.empty()) {
      return std::nullopt;
    }
    return account_from(ask("LOOKUP " + client_uuid + "\n"), "LOOKUP");
  }

  std::optional<std::string> start(
    const std::string &client_uuid,
    const std::vector<std::string> &arguments,
    const std::vector<std::string> &environment,
    const std::string &scope
  ) {
    if (client_uuid.empty() || arguments.empty()) {
      return std::nullopt;
    }

    std::string request = "START " + client_uuid + "\n";
    for (const auto &argument : arguments) {
      // A newline in an argument would end the line the broker is reading and
      // turn the rest into a request of its own. Nothing Hermes builds contains
      // one, so this is a refusal rather than an escape.
      if (argument.find('\n') != std::string::npos) {
        BOOST_LOG(error) << "[session-broker] Refusing to send an argument containing a newline.";
        return std::nullopt;
      }
      request += "ARG " + argument + "\n";
    }
    for (const auto &assignment : environment) {
      if (assignment.find('\n') != std::string::npos) {
        BOOST_LOG(error) << "[session-broker] Refusing to send an environment value "
                            "containing a newline.";
        return std::nullopt;
      }
      request += "ENV " + assignment + "\n";
    }
    if (!scope.empty()) {
      request += "SCOPE " + scope + "\n";
    }
    request += "END\n";

    const auto parts = succeeded(ask(request), "START");
    if (!parts || parts->empty()) {
      return std::nullopt;
    }
    BOOST_LOG(info) << "[session-broker] Started " << parts->front() << " for client "
                    << client_uuid << '.';
    return parts->front();
  }

  std::optional<std::string> wayland_socket(const std::string &client_uuid) {
    if (client_uuid.empty()) {
      return std::nullopt;
    }
    // Quiet on failure: this is polled while a compositor starts, and for most
    // of that time the honest answer is "not yet".
    const auto reply = ask("SOCKET " + client_uuid + "\n");
    if (!reply) {
      return std::nullopt;
    }
    const auto parts = fields(*reply);
    if (parts.size() < 2 || parts.front() != "OK") {
      return std::nullopt;
    }
    return parts[1];
  }

  bool active(const std::string &client_uuid) {
    if (client_uuid.empty()) {
      return false;
    }
    const auto parts = succeeded(ask("STATUS " + client_uuid + "\n"), "STATUS");
    if (!parts || parts->empty()) {
      return false;
    }
    // A unit that is still starting is running as far as the caller is
    // concerned. Reading only `active` here would report a compositor that
    // systemd has not finished launching as one that already died, and the
    // launch would be abandoned a few milliseconds before it succeeded.
    const auto &state = parts->front();
    return state == "active" || state == "activating" || state == "reloading";
  }

  bool stop(const std::string &client_uuid) {
    if (client_uuid.empty()) {
      return false;
    }
    return succeeded(ask("STOP " + client_uuid + "\n"), "STOP").has_value();
  }

}  // namespace platf::session_broker
