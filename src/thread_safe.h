/**
 * @file src/thread_safe.h
 * @brief Declarations for thread-safe data structures.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

// local includes
#include "utility.h"

namespace safe {
  struct queue_stats_t {
    bool running = false;
    std::size_t depth = 0;
    std::size_t capacity = 0;
    std::size_t high_watermark = 0;
    uint64_t overflow_events = 0;
    uint64_t dropped_elements = 0;
  };

  template<class T>
  class event_t {
  public:
    using status_t = util::optional_t<T>;

    struct raise_result_t {
      bool accepted = false;
      bool replaced = false;
    };

    template<class... Args>
    raise_result_t raise(Args &&...args) {
      std::lock_guard lg {_lock};
      if (!_continue) {
        return {};
      }

      const bool replaced = static_cast<bool>(_status);
      if constexpr (std::is_same_v<std::optional<T>, status_t>) {
        _status = std::make_optional<T>(std::forward<Args>(args)...);
      } else {
        _status = status_t {std::forward<Args>(args)...};
      }

      _cv.notify_all();
      return {
        .accepted = true,
        .replaced = replaced,
      };
    }

    // pop and view should not be used interchangeably
    status_t pop() {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      return val;
    }

    // pop and view should not be used interchangeably
    template<class Rep, class Period>
    status_t pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        if (!_continue || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      return val;
    }

    // pop and view should not be used interchangeably
    status_t view() {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    // pop and view should not be used interchangeably
    template<class Rep, class Period>
    status_t view(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        if (!_continue || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    bool peek() {
      return _continue && (bool) _status;
    }

    void stop() {
      std::lock_guard lg {_lock};

      _continue = false;

      _cv.notify_all();
    }

    void reset() {
      std::lock_guard lg {_lock};

      _continue = true;

      _status = util::false_v<status_t>;
    }

    [[nodiscard]] bool running() const {
      return _continue;
    }

  private:
    bool _continue {true};
    status_t _status {util::false_v<status_t>};

    std::condition_variable _cv;
    std::mutex _lock;
  };

  template<class T>
  class alarm_raw_t {
  public:
    using status_t = util::optional_t<T>;

    void ring(const status_t &status) {
      std::lock_guard lg(_lock);

      _status = status;
      _rang = true;
      _cv.notify_one();
    }

    void ring(status_t &&status) {
      std::lock_guard lg(_lock);

      _status = std::move(status);
      _rang = true;
      _cv.notify_one();
    }

    template<class Rep, class Period>
    auto wait_for(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this]() {
        return _rang;
      });
    }

    template<class Rep, class Period, class Pred>
    auto wait_for(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this, &pred]() {
        return _rang || pred();
      });
    }

    template<class Rep, class Period>
    auto wait_until(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this]() {
        return _rang;
      });
    }

    template<class Rep, class Period, class Pred>
    auto wait_until(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this, &pred]() {
        return _rang || pred();
      });
    }

    auto wait() {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this]() {
        return _rang;
      });
    }

    template<class Pred>
    auto wait(Pred &&pred) {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this, &pred]() {
        return _rang || pred();
      });
    }

    const status_t &status() const {
      return _status;
    }

    status_t &status() {
      return _status;
    }

    void reset() {
      _status = status_t {};
      _rang = false;
    }

  private:
    std::mutex _lock;
    std::condition_variable _cv;

    status_t _status {util::false_v<status_t>};
    bool _rang {false};
  };

  template<class T>
  using alarm_t = std::shared_ptr<alarm_raw_t<T>>;

  template<class T>
  alarm_t<T> make_alarm() {
    return std::make_shared<alarm_raw_t<T>>();
  }

  template<class T>
  class queue_t {
  public:
    using status_t = util::optional_t<T>;

    struct raise_result_t {
      bool accepted = false;
      std::size_t dropped = 0;
    };

    struct prioritized_raise_result_t {
      bool accepted = false;
      std::size_t superseded = 0;
      std::size_t overflow_dropped = 0;
    };

    struct overflow_stats_t {
      uint64_t events = 0;
      uint64_t dropped_elements = 0;
    };

    queue_t(std::uint32_t max_elements = 32):
        _max_elements {max_elements} {
    }

    template<class... Args>
    raise_result_t raise(Args &&...args) {
      return raise_with_overflow_handler(
        [](T &) {},
        std::forward<Args>(args)...
      );
    }

    /**
     * @brief Enqueue one element and inspect anything discarded on overflow.
     *
     * The handler runs under the queue lock immediately before the bounded
     * queue is cleared. Keep it non-blocking and do not call back into this
     * queue. Expensive accounting should be deferred until this call returns.
     */
    template<class OverflowHandler, class... Args>
    raise_result_t raise_with_overflow_handler(OverflowHandler &&handler, Args &&...args) {
      std::lock_guard ul {_lock};

      if (!_continue) {
        return {};
      }

      std::size_t dropped = 0;
      if (_queue.size() == _max_elements) {
        dropped = _queue.size();
        for (auto &element : _queue) {
          std::invoke(handler, element);
        }
        _queue.clear();
        ++_overflow_stats.events;
        _overflow_stats.dropped_elements += dropped;
      }

      _queue.emplace_back(std::forward<Args>(args)...);
      _high_watermark = std::max(_high_watermark, _queue.size());

      _cv.notify_all();
      return {
        .accepted = true,
        .dropped = dropped,
      };
    }

    /**
     * @brief Atomically supersede selected entries and enqueue at the front.
     *
     * This preserves FIFO order for entries that do not match the predicate.
     * If the queue remains full after cleanup, the normal bounded overflow
     * behavior still clears it before the priority element is inserted.
     */
    template<
      class SupersedePredicate,
      class SupersedeHandler,
      class OverflowHandler,
      class... Args
    >
    prioritized_raise_result_t raise_prioritized_with_cleanup(
      SupersedePredicate &&predicate,
      SupersedeHandler &&supersede_handler,
      OverflowHandler &&overflow_handler,
      Args &&...args
    ) {
      std::lock_guard ul {_lock};

      if (!_continue) {
        return {};
      }

      std::size_t superseded = 0;
      for (auto it = _queue.begin(); it != _queue.end();) {
        if (std::invoke(predicate, *it)) {
          std::invoke(supersede_handler, *it);
          it = _queue.erase(it);
          ++superseded;
        } else {
          ++it;
        }
      }

      std::size_t overflow_dropped = 0;
      if (_queue.size() == _max_elements) {
        overflow_dropped = _queue.size();
        for (auto &element : _queue) {
          std::invoke(overflow_handler, element);
        }
        _queue.clear();
        ++_overflow_stats.events;
        _overflow_stats.dropped_elements += overflow_dropped;
      }

      _queue.emplace(_queue.begin(), std::forward<Args>(args)...);
      _high_watermark = std::max(_high_watermark, _queue.size());

      _cv.notify_all();
      return {
        .accepted = true,
        .superseded = superseded,
        .overflow_dropped = overflow_dropped,
      };
    }

    [[nodiscard]] overflow_stats_t overflow_stats() const {
      std::lock_guard lg {_lock};
      return _overflow_stats;
    }

    [[nodiscard]] queue_stats_t stats() const {
      std::lock_guard lg {_lock};
      return {
        .running = _continue,
        .depth = _queue.size(),
        .capacity = _max_elements,
        .high_watermark = _high_watermark,
        .overflow_events = _overflow_stats.events,
        .dropped_elements = _overflow_stats.dropped_elements,
      };
    }

    bool peek() {
      return _continue && !_queue.empty();
    }

    template<class Rep, class Period>
    status_t pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        if (!_continue || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));

      return val;
    }

    status_t pop() {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));

      return val;
    }

    std::vector<T> &unsafe() {
      return _queue;
    }

    void stop() {
      std::lock_guard lg {_lock};

      _continue = false;

      _cv.notify_all();
    }

    [[nodiscard]] bool running() const {
      return _continue;
    }

  private:
    bool _continue {true};
    std::uint32_t _max_elements;
    std::size_t _high_watermark = 0;
    overflow_stats_t _overflow_stats;

    mutable std::mutex _lock;
    std::condition_variable _cv;

    std::vector<T> _queue;
  };

  template<class T>
  class shared_t {
  public:
    using element_type = T;

    using construct_f = std::function<int(element_type &)>;
    using destruct_f = std::function<void(element_type &)>;

    struct ptr_t {
      shared_t *owner;

      ptr_t():
          owner {nullptr} {
      }

      explicit ptr_t(shared_t *owner):
          owner {owner} {
      }

      ptr_t(ptr_t &&ptr) noexcept:
          owner {ptr.owner} {
        ptr.owner = nullptr;
      }

      ptr_t(const ptr_t &ptr) noexcept:
          owner {ptr.owner} {
        if (!owner) {
          return;
        }

        auto tmp = ptr.owner->ref();
        tmp.owner = nullptr;
      }

      ptr_t &operator=(const ptr_t &ptr) noexcept {
        if (!ptr.owner) {
          release();

          return *this;
        }

        return *this = std::move(*ptr.owner->ref());
      }

      ptr_t &operator=(ptr_t &&ptr) noexcept {
        if (owner) {
          release();
        }

        std::swap(owner, ptr.owner);

        return *this;
      }

      ~ptr_t() {
        if (owner) {
          release();
        }
      }

      operator bool() const {
        return owner != nullptr;
      }

      void release() {
        std::lock_guard lg {owner->_lock};

        if (!--owner->_count) {
          owner->_destruct(*get());
          (*this)->~element_type();
        }

        owner = nullptr;
      }

      element_type *get() const {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }

      element_type *operator->() {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }
    };

    template<class FC, class FD>
    shared_t(FC &&fc, FD &&fd):
        _construct {std::forward<FC>(fc)},
        _destruct {std::forward<FD>(fd)} {
    }

    [[nodiscard]] ptr_t ref() {
      std::lock_guard lg {_lock};

      if (!_count) {
        new (_object_buf.data()) element_type;
        if (_construct(*reinterpret_cast<element_type *>(_object_buf.data()))) {
          return ptr_t {nullptr};
        }
      }

      ++_count;

      return ptr_t {this};
    }

  private:
    construct_f _construct;
    destruct_f _destruct;

    std::array<std::uint8_t, sizeof(element_type)> _object_buf;

    std::uint32_t _count;
    std::mutex _lock;
  };

  template<class T, class F_Construct, class F_Destruct>
  auto make_shared(F_Construct &&fc, F_Destruct &&fd) {
    return shared_t<T> {
      std::forward<F_Construct>(fc),
      std::forward<F_Destruct>(fd)
    };
  }

  using signal_t = event_t<bool>;

  class mail_raw_t;
  using mail_t = std::shared_ptr<mail_raw_t>;

  void cleanup(mail_raw_t *);

  template<class T>
  class post_t: public T {
  public:
    template<class... Args>
    post_t(mail_t mail, Args &&...args):
        T(std::forward<Args>(args)...),
        mail {std::move(mail)} {
    }

    mail_t mail;

    ~post_t() {
      cleanup(mail.get());
    }
  };

  template<class T>
  inline auto lock(const std::weak_ptr<void> &wp) {
    return std::reinterpret_pointer_cast<typename T::element_type>(wp.lock());
  }

  class mail_raw_t: public std::enable_shared_from_this<mail_raw_t> {
  public:
    template<class T>
    using event_t = std::shared_ptr<post_t<event_t<T>>>;

    template<class T>
    using queue_t = std::shared_ptr<post_t<queue_t<T>>>;

    template<class T>
    event_t<T> event(const std::string_view &id) {
      std::lock_guard lg {mutex};

      auto it = id_to_post.find(id);
      if (it != std::end(id_to_post)) {
        return lock<event_t<T>>(it->second);
      }

      auto post = std::make_shared<typename event_t<T>::element_type>(shared_from_this());
      id_to_post.emplace(std::pair<std::string, std::weak_ptr<void>> {std::string {id}, post});

      return post;
    }

    template<class T>
    queue_t<T> queue(const std::string_view &id) {
      std::lock_guard lg {mutex};

      auto it = id_to_post.find(id);
      if (it != std::end(id_to_post)) {
        return lock<queue_t<T>>(it->second);
      }

      auto post = std::make_shared<typename queue_t<T>::element_type>(shared_from_this(), 32);
      id_to_post.emplace(std::pair<std::string, std::weak_ptr<void>> {std::string {id}, post});

      return post;
    }

    void cleanup() {
      std::lock_guard lg {mutex};

      for (auto it = std::begin(id_to_post); it != std::end(id_to_post); ++it) {
        auto &weak = it->second;

        if (weak.expired()) {
          id_to_post.erase(it);

          return;
        }
      }
    }

    std::mutex mutex;

    std::map<std::string, std::weak_ptr<void>, std::less<>> id_to_post;
  };

  inline void cleanup(mail_raw_t *mail) {
    mail->cleanup();
  }
}  // namespace safe
