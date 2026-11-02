// SPDX-License-Identifier: GPL-3.0-only
/** @file session_broker_transport.cpp
 *  @brief Nonblocking session-broker framing and socket deadlines.
 */
#include "session_broker_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace platf::session_broker::detail {
  namespace {
    using clock = std::chrono::steady_clock;

    struct owned_fd {
      int value;

      ~owned_fd() {
        if (value >= 0) {
          ::close(value);
        }
      }
    };

    int remaining(clock::time_point deadline) {
      const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
      return static_cast<int>(std::clamp<decltype(count)>(count, 0, std::numeric_limits<int>::max()));
    }

    bool wait_ready(int fd, short events, clock::time_point deadline) {
      for (;;) {
        const auto milliseconds = remaining(deadline);
        if (milliseconds == 0) {
          errno = ETIMEDOUT;
          return false;
        }
        pollfd descriptor {fd, events, 0};
        const auto result = ::poll(&descriptor, 1, milliseconds);
        if (result > 0) {
          return (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
        }
        if (result == 0) {
          errno = ETIMEDOUT;
          return false;
        }
        if (errno != EINTR) {
          return false;
        }
      }
    }

    bool connect_socket(int fd, const sockaddr_un &address, clock::time_point deadline) {
      while (remaining(deadline) > 0) {
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 || errno == EISCONN) {
          return true;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // AF_UNIX has not connected when its listen queue is full. POLLOUT
          // and SO_ERROR=0 alone would incorrectly call this a connected fd.
          ::poll(nullptr, 0, std::min(5, remaining(deadline)));
          continue;
        }
        if (errno != EINPROGRESS || !wait_ready(fd, POLLOUT, deadline)) {
          return false;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
          return false;
        }
        if (error != 0) {
          errno = error;
          return false;
        }
        return true;
      }
      errno = ETIMEDOUT;
      return false;
    }

    exchange_result failure(const char *operation) {
      return {.reply = std::nullopt, .error = std::string {operation} + ": " + std::strerror(errno)};
    }
  }  // namespace

  std::chrono::milliseconds request_timeout(std::string_view request) {
    const auto verb = request.substr(0, request.find_first_of(" \n"));
    if (verb == "ENSURE" || verb == "START" || verb == "STOP" || verb == "PURGE") {
      return std::chrono::minutes(3);
    }
    return std::chrono::seconds(15);
  }

  exchange_result exchange(const std::string &path, const std::string &request, std::chrono::milliseconds timeout) {
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path) || path.find('\0') != std::string::npos) {
      return {.reply = std::nullopt, .error = "invalid Unix socket path"};
    }
    if (request.empty() || request.size() > 64 * 1024 || request.back() != '\n' || request.find('\0') != std::string::npos) {
      return {.reply = std::nullopt, .error = "invalid or oversized request"};
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    owned_fd fd {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (fd.value < 0) {
      return failure("create socket");
    }
    // Keep queued request data bounded per connection. Large environments
    // stream through this buffer; nonblocking writes still share the deadline.
    const int send_buffer = 16 * 1024;
    ::setsockopt(fd.value, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    const auto deadline = clock::now() + timeout;
    if (!connect_socket(fd.value, address, deadline)) {
      return failure("connect");
    }
    for (std::size_t offset = 0; offset < request.size();) {
      if (!wait_ready(fd.value, POLLOUT, deadline)) {
        return failure("send request");
      }
      const auto count = ::send(fd.value, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (count <= 0) {
        return failure("send request");
      }
      offset += static_cast<std::size_t>(count);
    }
    ::shutdown(fd.value, SHUT_WR);

    constexpr std::size_t maximum_reply = 4096;
    std::string reply;
    std::array<char, 512> buffer {};
    while (reply.size() < maximum_reply) {
      if (!wait_ready(fd.value, POLLIN, deadline)) {
        return failure("read reply");
      }
      const auto count = ::read(fd.value, buffer.data(), std::min(buffer.size(), maximum_reply - reply.size()));
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (count < 0) {
        return failure("read reply");
      }
      if (count == 0) {
        return {.reply = std::nullopt, .error = "broker closed before a complete reply"};
      }
      reply.append(buffer.data(), static_cast<std::size_t>(count));
      if (const auto newline = reply.find('\n'); newline != std::string::npos) {
        reply.resize(newline);
        if (reply.empty() || reply.find('\0') != std::string::npos) {
          return {.reply = std::nullopt, .error = "empty or malformed broker reply"};
        }
        return {.reply = std::move(reply), .error = {}};
      }
    }
    return {.reply = std::nullopt, .error = "broker reply exceeds 4096 bytes"};
  }
}  // namespace platf::session_broker::detail
