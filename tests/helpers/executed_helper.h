#pragma once

#ifdef __linux__
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace test {
  // posix_spawn() can return before the exec's close-on-exec work has finished
  // on the child's kernel path. An acknowledgement from the new userspace
  // image proves that inherited CLOEXEC descriptors have actually been closed.
  class executed_helper_t {
  public:
    executed_helper_t() = default;
    executed_helper_t(const executed_helper_t &) = delete;
    executed_helper_t &operator=(const executed_helper_t &) = delete;

    ~executed_helper_t() {
      if (child_ > 0) {
        ::kill(child_, SIGTERM);
        while (::waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
      }
    }

    int start() {
      if (child_ > 0) return EALREADY;
      int ready[2];
      if (::pipe2(ready, O_CLOEXEC) != 0) return errno;

      posix_spawn_file_actions_t actions;
      int error = ::posix_spawn_file_actions_init(&actions);
      if (error == 0) {
        error = ::posix_spawn_file_actions_adddup2(&actions, ready[1], STDOUT_FILENO);
        if (error == 0) {
          char shell[] = "/bin/sh";
          char option[] = "-c";
          char command[] = "printf R; exec /bin/sleep 30";
          char *arguments[] = {shell, option, command, nullptr};
          error = ::posix_spawn(&child_, shell, &actions, nullptr, arguments, ::environ);
        }
        ::posix_spawn_file_actions_destroy(&actions);
      }
      ::close(ready[1]);
      if (error == 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
          const auto left = deadline - std::chrono::steady_clock::now();
          if (left <= std::chrono::steady_clock::duration::zero()) {
            error = ETIMEDOUT;
            break;
          }
          const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
          pollfd descriptor {ready[0], POLLIN, 0};
          const int result = ::poll(&descriptor, 1, static_cast<int>(std::max<long long>(1, milliseconds)));
          if (result < 0 && errno == EINTR) continue;
          if (result <= 0) {
            error = result == 0 ? ETIMEDOUT : errno;
            break;
          }
          char token = 0;
          const auto bytes = ::read(ready[0], &token, 1);
          if (bytes < 0 && errno == EINTR) continue;
          if (bytes != 1 || token != 'R') error = EIO;
          break;
        }
      }
      ::close(ready[0]);
      return error;
    }

    bool running() {
      if (child_ <= 0) return false;
      pid_t result;
      do { result = ::waitpid(child_, nullptr, WNOHANG); } while (result < 0 && errno == EINTR);
      if (result == 0) return true;
      child_ = -1;
      return false;
    }

  private:
    pid_t child_ {-1};
  };
}
#endif
