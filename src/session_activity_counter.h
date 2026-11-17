/**
 * @file src/session_activity_counter.h
 * @brief Serialized lifecycle transitions for platform-wide streaming state.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <mutex>
#include <utility>

namespace stream {

  // Calls the platform hooks only at 0->1 and 1->0. Holding this mutex across
  // the hook prevents a concurrent start from racing the last disconnect.
  // Platform hooks must therefore be non-blocking.
  class session_activity_counter_t {
  public:
    template<typename Callback>
    void started(Callback &&on_first) {
      std::lock_guard lock {mutex_};
      if (++count_ == 1) {
        std::forward<Callback>(on_first)();
      }
    }

    template<typename Callback>
    void stopped(Callback &&on_last) {
      std::lock_guard lock {mutex_};
      assert(count_ > 0 && "Unbalanced session activity stop");
      if (count_ == 0) {
        return;
      }
      if (--count_ == 0) {
        std::forward<Callback>(on_last)();
      }
    }

    std::size_t count() const {
      std::lock_guard lock {mutex_};
      return count_;
    }

  private:
    mutable std::mutex mutex_;
    std::size_t count_ {0};
  };

}  // namespace stream
