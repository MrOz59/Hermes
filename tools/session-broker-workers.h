// SPDX-License-Identifier: GPL-3.0-only
/** @file session-broker-workers.h
 *  @brief Bounded, independently reaped workers for the single-threaded broker.
 */
#pragma once

#include <cerrno>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

namespace hermes::session_broker {
  /**
   * @brief Keep slow account/systemd work out of the socket accept loop.
   *
   * Call only from a single-threaded parent. Children receive a process-local
   * snapshot, close the supplied parent descriptors and exit without running
   * parent destructors. Reaping touches only children owned by this pool.
   */
  class worker_pool {
  public:
    /** @param maximum Maximum live request and reconciliation workers. */
    explicit worker_pool(std::size_t maximum):
        maximum_(maximum) {}

    worker_pool(const worker_pool &) = delete;
    worker_pool &operator=(const worker_pool &) = delete;

    ~worker_pool() {
      reap();
      for (const auto child : children_) {
        ::kill(child, SIGKILL);
      }
      for (const auto child : children_) {
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
      }
    }

    /** @brief Reap completed workers without consuming another owner's child. */
    void reap() {
      for (auto it = children_.begin(); it != children_.end();) {
        const auto result = ::waitpid(*it, nullptr, WNOHANG);
        if (result > 0 || (result < 0 && errno == ECHILD)) {
          it = children_.erase(it);
        } else {
          ++it;
        }
      }
    }

    /** @return Whether a worker is still tracked (call reap before checking). */
    bool contains(pid_t child) const {
      return children_.contains(child);
    }

    /**
     * @brief Fork a bounded worker, or return -1 without running the job.
     * @param close_in_child Parent-only descriptors, especially the listener.
     * @param job Work executed only in the child.
     * @return Child PID in the parent; -1 for exhaustion or fork failure.
     */
    pid_t spawn(std::initializer_list<int> close_in_child, const std::function<void()> &job) {
      reap();
      if (children_.size() >= maximum_) {
        return -1;
      }
      const auto child = ::fork();
      if (child == 0) {
        for (const auto fd : close_in_child) {
          ::close(fd);
        }
        try {
          job();
          ::_exit(0);
        } catch (...) {
          std::fputs("session broker worker failed\n", stderr);
          ::_exit(1);
        }
      }
      if (child > 0) {
        children_.insert(child);
      }
      return child;
    }

  private:
    std::size_t maximum_;
    std::unordered_set<pid_t> children_;
  };
}  // namespace hermes::session_broker
