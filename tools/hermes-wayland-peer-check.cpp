// SPDX-License-Identifier: GPL-3.0-only
// Standalone regression-test consumer for scripts/test-wayland-peer.py.
#include "../src/platform/linux/wayland_peer.h"

#include <iostream>

int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }
  if (::prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0) {
    return 3;
  }
  __user_cap_header_struct header {_LINUX_CAPABILITY_VERSION_3, 0};
  __user_cap_data_struct before[2] {};
  if (::syscall(SYS_capget, &header, before) != 0) {
    return 4;
  }
  const std::string mode = argv[1];
  int outcome = 0;
  {
    platf::kwin::wayland_peer_t peer;
    std::string error;
    int fd = mode == "direct" ? platf::kwin::connect_session_socket(argv[3], error) : peer.open_with_helper(argv[3], argv[2], error);
    if (mode == "failure") {
      if (fd >= 0 || error.empty()) {
        if (fd >= 0) {
          ::close(fd);
        }
        return 5;
      }
      std::cout << error << '\n';
    } else if (fd < 0) {
      std::cerr << error << '\n';
      return 6;
    } else {
      if (::fcntl(fd, F_GETFD) != FD_CLOEXEC || ::send(fd, "ready", 5, MSG_NOSIGNAL) != 5) {
        outcome = 7;
      } else {
        pollfd event {fd, POLLIN, 0};
        char response[2] {};
        if (::poll(&event, 1, 5000) <= 0 || ::recv(fd, response, 2, 0) != 2 || std::memcmp(response, "ok", 2) != 0) {
          outcome = 8;
        }
      }
      ::close(fd);
    }
  }
  __user_cap_data_struct after[2] {};
  if (::syscall(SYS_capget, &header, after) != 0 || std::memcmp(before, after, sizeof(before)) != 0 || ::prctl(PR_GET_DUMPABLE) != 0) {
    return 9;
  }
  int status;
  if (::waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) {
    return 10;
  }
  return outcome;
}
