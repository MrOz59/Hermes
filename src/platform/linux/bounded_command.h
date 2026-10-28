/**
 * @file src/platform/linux/bounded_command.h
 * @brief Capture a shell helper's output without an unbounded shutdown wait.
 */
#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <optional>
#include <string>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace platf::bounded_command {
  inline std::optional<std::string> output(const std::string &command, std::chrono::milliseconds timeout, bool *timed_out) {
    if (timed_out) {
      *timed_out = false;
    }
    int out[2];
    if (::pipe2(out, O_CLOEXEC) != 0) {
      return std::nullopt;
    }
    const pid_t child = ::fork();
    if (child < 0) {
      ::close(out[0]);
      ::close(out[1]);
      return std::nullopt;
    }
    if (child == 0) {
      // Its own process group, so that running over takes down whatever the
      // shell started along with the shell.
      ::setpgid(0, 0);
      ::dup2(out[1], STDOUT_FILENO);
      ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
      ::_exit(127);
    }
    ::setpgid(child, child);  // Also close the fork-to-setpgid race from the parent.
    ::close(out[1]);

    std::string output;
    std::array<char, 4096> buffer {};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool pipe_open = true;
    bool exited = false;
    bool expired = false;
    int status = -1;
    while (pipe_open || !exited) {
      if (std::chrono::steady_clock::now() >= deadline) {
        expired = true;
        break;
      }
      if (pipe_open) {
        // Once the command has exited, whatever is still in the pipe is all
        // there will be: a descendant that kept it open is not waited for.
        pollfd readable {out[0], POLLIN, 0};
        const int ready = ::poll(&readable, 1, exited ? 0 : 20);
        if (ready > 0) {
          const ssize_t got = ::read(out[0], buffer.data(), buffer.size());
          if (got > 0) {
            output.append(buffer.data(), static_cast<size_t>(got));
          } else if (got == 0 || errno != EINTR) {
            pipe_open = false;
          }
        } else if (exited && ready == 0) {
          pipe_open = false;
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
      }
      if (!exited) {
        const pid_t reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == child) {
          exited = true;
        } else if (reaped < 0 && errno != EINTR) {
          exited = true;  // Not ours to wait for any more; the status is unknown.
          status = -1;
        }
      }
    }
    ::close(out[0]);

    if (expired) {
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
      if (!exited) {
        // SIGKILL cannot immediately finish a task waiting in the kernel.
        // Never turn a command timeout into an unbounded waitpid under the
        // display locks. The ordinary child reaper collects a late exit.
        const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {100};
        do {
          const auto reaped = ::waitpid(child, &status, WNOHANG);
          if (reaped == child || (reaped < 0 && errno != EINTR)) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds {1});
        } while (std::chrono::steady_clock::now() < reap_deadline);
      }
      if (timed_out) {
        *timed_out = true;
      }
      return std::nullopt;
    }
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return std::nullopt;
    }
    return output;
  }

}  // namespace platf::bounded_command
