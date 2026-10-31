/** @file session_socket.h
 *  @brief Explicit, bounded Unix socket connection without display or remote environment lookup.
 */
#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace platf::kwin {
  inline int connect_session_socket(const std::string &path, std::string &error) {
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos || path.size() >= sizeof(address.sun_path)) {
      error = "A private session connection requires an absolute Unix socket path";
      return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      error = std::strerror(errno);
      return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) {
      return fd;
    }
    // AF_UNIX returns EAGAIN for a full accept queue; it is not a completed
    // connection. Never turn that condition into a connection to another socket.
    if (errno == EINPROGRESS) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      pollfd event {fd, POLLOUT, 0};
      int result;
      do {
        result = ::poll(&event, 1, static_cast<int>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count())));
      } while (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);
      int socket_error = 0;
      socklen_t size = sizeof(socket_error);
      if (result > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) == 0 && socket_error == 0) {
        return fd;
      }
      error = result == 0 ? "Private session connection timed out" :
                            std::strerror(socket_error ? socket_error : errno);
    } else {
      error = std::strerror(errno);
    }
    ::close(fd);
    return -1;
  }

}  // namespace platf::kwin
