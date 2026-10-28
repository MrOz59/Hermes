/**
 * @file src/platform/linux/process_group.h
 * @brief Non-destructive process-group liveness checks and bounded cleanup helpers.
 */
#pragma once

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <limits>
#include <sys/wait.h>
#include <unistd.h>

namespace platf::process_group {
  inline bool valid(std::uintptr_t handle) {
    // Never allow PID 0, -1, or our own group to become a broadcast kill.
    return handle > 1 && handle <= static_cast<std::uintptr_t>(std::numeric_limits<pid_t>::max()) &&
           static_cast<pid_t>(handle) != getpgrp();
  }

  inline bool running(std::uintptr_t handle) {
    if (!valid(handle)) {
      return false;
    }
    int result;
    do {
      result = kill(-static_cast<pid_t>(handle), 0);
    } while (result < 0 && errno == EINTR);
    // waitpid(-pgid) only sees our direct children. It returns ECHILD after a
    // launcher exits even if Gamescope/Wine descendants still occupy the group.
    // Signal 0 probes the entire group without reaping a child's exit status.
    return result == 0 || errno != ESRCH;
  }

  inline bool signal(std::uintptr_t handle, int sig) {
    if (!valid(handle)) {
      errno = EINVAL;
      return false;
    }
    int result;
    do {
      result = kill(-static_cast<pid_t>(handle), sig);
    } while (result < 0 && errno == EINTR);
    return result == 0 || errno == ESRCH;
  }

  // Only use during termination, when the application exit status is no longer
  // needed. Liveness checks during normal streaming must not consume it.
  inline void reap_children(std::uintptr_t handle) {
    if (!valid(handle)) {
      return;
    }
    for (;;) {
      const auto result = waitpid(-static_cast<pid_t>(handle), nullptr, WNOHANG);
      if (result > 0 || (result < 0 && errno == EINTR)) {
        continue;
      }
      break;
    }
  }
}  // namespace platf::process_group
