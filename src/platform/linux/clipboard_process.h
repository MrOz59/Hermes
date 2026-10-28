/**
 * @file src/platform/linux/clipboard_process.h
 * @brief Clipboard helper pipes with isolated descriptor ownership.
 */
#pragma once

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <string_view>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace platf::clipboard {
  class pipe_t {
  public:
    // Resolve the executable in the parent. After fork, the child uses only
    // async-signal-safe operations before execv, even in a multithreaded host.
    pipe_t(const char *executable, const char *const arguments[], bool writing) {
      const long max_fd = sysconf(_SC_OPEN_MAX);
      if (max_fd < 0) {
        return;
      }
      int descriptors[2];
      if (pipe2(descriptors, O_CLOEXEC) != 0) {
        return;
      }
      // Keep the pipe out of stdin/stdout/stderr, including when one of the
      // standard descriptors was closed before Hermes started.
      for (int &fd : descriptors) {
        if (fd < 3) {
          const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
          if (replacement < 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            return;
          }
          close(fd);
          fd = replacement;
        }
      }

      child = fork();
      if (child == 0) {
        const int source = descriptors[writing ? 0 : 1];
        const int target = writing ? STDIN_FILENO : STDOUT_FILENO;
        if (dup2(source, target) < 0) {
          _exit(127);
        }
        // popen() only closes its own pipes. wl-copy and xclip daemonize,
        // which otherwise leaves every inherited stream socket alive (#71).
        // Close in the child, so concurrent socket creation in the parent
        // cannot escape a snapshot of its descriptor table.
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
        {
          for (long fd = 3; fd < max_fd; ++fd) {
            close(static_cast<int>(fd));
          }
        }
        execv(executable, const_cast<char *const *>(arguments));
        _exit(127);
      }

      close(descriptors[writing ? 0 : 1]);
      const int parent_fd = descriptors[writing ? 1 : 0];
      if (child < 0) {
        close(parent_fd);
        return;
      }
      stream = fdopen(parent_fd, writing ? "w" : "r");
      if (!stream) {
        close(parent_fd);
        finish();
      }
    }

    pipe_t(const pipe_t &) = delete;
    pipe_t &operator=(const pipe_t &) = delete;

    ~pipe_t() {
      finish();
    }

    FILE *get() const {
      return stream;
    }

    bool finish() {
      const int close_status = stream ? fclose(stream) : 0;
      stream = nullptr;
      if (child < 0) {
        return false;
      }
      int status = 0;
      pid_t result;
      do {
        result = waitpid(child, &status, 0);
      } while (result < 0 && errno == EINTR);
      child = -1;
      return close_status == 0 && result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    bool write(std::string_view content) {
      if (!stream) {
        return false;
      }
      // A missing/crashing helper must not terminate Hermes with SIGPIPE.
      // Keep the mask through fclose(), which can flush buffered bytes.
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGPIPE);
      sigset_t previous_mask;
      if (pthread_sigmask(SIG_BLOCK, &mask, &previous_mask) != 0) {
        return false;
      }
      sigset_t pending;
      const bool had_pending_sigpipe = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
      const bool wrote_all = fwrite(content.data(), 1, content.size(), stream) == content.size();
      const bool closed = finish();
      if (!had_pending_sigpipe) {
        const timespec no_wait {};
        while (sigtimedwait(&mask, nullptr, &no_wait) < 0 && errno == EINTR) {}
      }
      pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
      return wrote_all && closed;
    }

  private:
    FILE *stream = nullptr;
    pid_t child = -1;
  };
}  // namespace platf::clipboard
