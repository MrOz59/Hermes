/**
 * @file wayland_peer.h
 * @brief Give private KWin connections an executable identity without changing Hermes's capabilities.
 */
#pragma once

#include "session_socket.h"

#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace platf::kwin {
  // Internal mode of hermes-detached-session, entered before desktop startup.
  // This is a fresh exec: making the helper dumpable must never expose a forked
  // copy of the capability-bearing Hermes process's address space.
  inline int serve_wayland_peer(const char *path) {
    constexpr int channel = 3;
    if (::getuid() != ::geteuid() || ::getgid() != ::getegid()) {
      return 1;
    }
    __user_cap_header_struct header {_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct capabilities[2] {};
    if (::syscall(SYS_capset, &header, capabilities) < 0 ||
        ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0L, 0L, 0L) < 0 ||
        ::prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) < 0 ||
        ::prctl(PR_SET_DUMPABLE, 1L, 0L, 0L, 0L) < 0) {
      return 1;
    }
    int type = 0;
    socklen_t size = sizeof(type);
    if (::getsockopt(channel, SOL_SOCKET, SO_TYPE, &type, &size) < 0 || type != SOCK_SEQPACKET) {
      return 1;
    }

    std::string error;
    const int connection = connect_session_socket(path, error);
    const char success = connection >= 0 ? 1 : 0;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
    iovec payload {const_cast<char *>(&success), sizeof(success)};
    msghdr message {};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    if (connection >= 0) {
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      auto *item = CMSG_FIRSTHDR(&message);
      item->cmsg_level = SOL_SOCKET;
      item->cmsg_type = SCM_RIGHTS;
      item->cmsg_len = CMSG_LEN(sizeof(connection));
      std::memcpy(CMSG_DATA(item), &connection, sizeof(connection));
    }
    const auto sent = ::sendmsg(channel, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (connection >= 0) {
      ::close(connection);
    }
    if (sent != sizeof(success) || !success) {
      return 1;
    }
    // KWin reads /proc/<peer-pid>/exe when it accepts the socket. Stay alive
    // until the parent's registry round trips complete, or the parent exits.
    // The parent also bounds setup and owns reaping us on every failure path.
    pollfd event {channel, POLLIN, 0};
    return ::poll(&event, 1, 30000) > 0 ? 0 : 1;
  }

  class wayland_peer_t {
  public:
    wayland_peer_t() = default;
    wayland_peer_t(const wayland_peer_t &) = delete;
    wayland_peer_t &operator=(const wayland_peer_t &) = delete;

    ~wayland_peer_t() {
      if (channel >= 0) {
        ::close(channel);
      }
      if (child > 0) {
        int status;
        pid_t result;
        do {
          result = ::waitpid(child, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
          // The setup has finished (or failed); there is no remaining work.
          // A stuck loader/helper must not stall shutdown or retain its PID.
          ::kill(child, SIGKILL);
          do {
            result = ::waitpid(child, &status, 0);
          } while (result < 0 && errno == EINTR);
        }
      }
    }

    // The returned socket belongs to libwayland/the caller. This guard must
    // live until KWin has accepted it and sent the initial registry responses.
    int open(const std::string &path, std::string &error) {
      std::error_code ec;
      auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
      if (ec) {
        error = "Could not locate Hermes's private Wayland connection helper: " + ec.message();
        return -1;
      }
      return open_with_helper(path, (executable.parent_path() / "hermes-detached-session").string(), error);
    }

    int open_with_helper(const std::string &path, const std::string &helper, std::string &error) {
      error.clear();
      sockaddr_un address {};
      if (child > 0 || path.empty() || path.front() != '/' || path.find('\0') != std::string::npos || path.size() >= sizeof(address.sun_path) ||
          helper.empty() || helper.front() != '/' || helper.find('\0') != std::string::npos) {
        error = "Private Wayland connection requires an absolute socket and helper path";
        return -1;
      }
      const long max_fd = ::sysconf(_SC_OPEN_MAX);
      if (max_fd < 0) {
        error = "Could not determine the descriptor limit for the private Wayland helper";
        return -1;
      }
      int descriptors[2];
      if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) != 0) {
        error = "Could not create private Wayland helper channel: " + std::string(std::strerror(errno));
        return -1;
      }
      // Keep the child's source away from fd 3, even with closed stdin/stdout.
      const int source = ::fcntl(descriptors[1], F_DUPFD_CLOEXEC, 4);
      ::close(descriptors[1]);
      if (source < 0) {
        ::close(descriptors[0]);
        error = "Could not prepare private Wayland helper descriptor";
        return -1;
      }
      char *const arguments[] {const_cast<char *>(helper.c_str()), const_cast<char *>("--connect-wayland"), const_cast<char *>(path.c_str()), nullptr};
      char *const environment[] {nullptr};
      child = ::fork();
      if (child == 0) {
        // Only async-signal-safe operations between fork and exec. In
        // particular, never make this copy of Hermes's memory dumpable.
        if (::dup2(source, 3) < 0) {
          ::_exit(127);
        }
        ::close(0);
        ::close(1);
        ::close(2);
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 4U, ~0U, 0U) != 0)
#endif
        {
          for (long fd = 4; fd < max_fd; ++fd) {
            ::close(static_cast<int>(fd));
          }
        }
        // No inherited loader variables, display defaults, credentials or
        // stream descriptors enter the helper. Its only job uses fd 3.
        ::execve(helper.c_str(), arguments, environment);
        ::_exit(127);
      }
      ::close(source);
      channel = descriptors[0];
      if (child < 0) {
        error = "Could not start private Wayland connection helper";
        return -1;
      }

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      pollfd event {channel, POLLIN, 0};
      int result;
      do {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        result = ::poll(&event, 1, static_cast<int>(std::max<std::int64_t>(0, left)));
      } while (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);
      if (result <= 0 || !(event.revents & POLLIN)) {
        error = "Private Wayland connection helper failed or timed out; check hermes-detached-session beside Hermes";
        return -1;
      }
      char success = 0;
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
      iovec payload {&success, sizeof(success)};
      msghdr message {};
      message.msg_iov = &payload;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      const auto received = ::recvmsg(channel, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
      int connection = -1;
      bool extra = false;
      for (auto *item = CMSG_FIRSTHDR(&message); received >= 0 && item; item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS && item->cmsg_len >= CMSG_LEN(0)) {
          const auto count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
          for (size_t i = 0; i < count; ++i) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(item) + i * sizeof(int), sizeof(fd));
            if (connection < 0) {
              connection = fd;
            } else {
              ::close(fd);
              extra = true;
            }
          }
        }
      }
      if (received != 1 || success != 1 || connection < 0 || extra || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        if (connection >= 0) {
          ::close(connection);
        }
        error = "Private Wayland helper could not connect to the requested socket; refusing host fallback";
        return -1;
      }
      return connection;
    }

  private:
    int channel {-1};
    pid_t child {-1};
  };
}  // namespace platf::kwin
