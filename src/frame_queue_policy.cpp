/**
 * @file src/frame_queue_policy.cpp
 * @brief H2 encoded-frame deadline and recovery policy.
 */

#include "frame_queue_policy.h"

#include <algorithm>

namespace stream::queueing {

  frame_queue_decision_t frame_queue_policy_t::evaluate(
    const frame_queue_request_t &request
  ) {
    std::lock_guard lock {mutex_};
    auto &entry = entry_for(request.session_key);
    auto decision = frame_queue_decision_t {
      .priority =
        priority::video_frame_priority(request.is_idr),
    };

    if (request.encoded_at && request.now > *request.encoded_at) {
      decision.queue_time = request.now - *request.encoded_at;
    }

    const auto recovery_queue_time =
      request.max_recovery_queue_time >
          std::chrono::microseconds::zero() ?
        request.max_recovery_queue_time :
        request.max_queue_time;
    if (
      entry.draining_recovery &&
      !request.is_idr &&
      request.encoded_at &&
      decision.queue_time <= request.max_queue_time
    ) {
      entry.draining_recovery = false;
    }
    decision.recovery_drain =
      request.is_idr || entry.draining_recovery;
    decision.admission_budget =
      decision.recovery_drain ?
        recovery_queue_time :
        request.max_queue_time;
    const auto deadline_enabled =
      decision.admission_budget >
        std::chrono::microseconds::zero();
    const auto deadline_expired =
      deadline_enabled &&
      request.encoded_at &&
      decision.queue_time > decision.admission_budget;

    if (request.is_idr) {
      if (deadline_expired) {
        entry.awaiting_idr = true;
        entry.draining_recovery = false;
        entry.recovery_cause =
          frame_recovery_cause_e::deadline_expired;
        decision.drop_reason =
          frame_queue_drop_reason_e::deadline_expired;
        decision.recovery_cause = entry.recovery_cause;
        decision.request_idr =
          allow_idr_request_locked(entry, request.now);
        entry.idr_requested = decision.request_idr;
        decision.idr_request_rate_limited =
          !decision.request_idr;
        return decision;
      }

      entry.awaiting_idr = false;
      entry.idr_requested = false;
      entry.draining_recovery = true;
      entry.recovery_cause = frame_recovery_cause_e::none;
      return decision;
    }

    if (entry.awaiting_idr) {
      decision.drop_reason =
        frame_queue_drop_reason_e::awaiting_recovery;
      decision.recovery_cause = entry.recovery_cause;
      if (!entry.idr_requested) {
        decision.request_idr =
          allow_idr_request_locked(entry, request.now);
        entry.idr_requested = decision.request_idr;
        decision.idr_request_rate_limited =
          !decision.request_idr;
      }
      return decision;
    }

    if (deadline_expired) {
      entry.awaiting_idr = true;
      entry.recovery_cause =
        frame_recovery_cause_e::deadline_expired;
      entry.draining_recovery = false;
      decision.drop_reason =
        frame_queue_drop_reason_e::deadline_expired;
      decision.recovery_cause = entry.recovery_cause;
      decision.request_idr =
        allow_idr_request_locked(entry, request.now);
      entry.idr_requested = decision.request_idr;
      decision.idr_request_rate_limited =
        !decision.request_idr;
    }

    return decision;
  }

  bool frame_queue_policy_t::allow_idr_request(
    const void *session_key,
    frame_queue_time_point_t now
  ) {
    std::lock_guard lock {mutex_};
    auto &entry = entry_for(session_key);
    const auto allowed = allow_idr_request_locked(entry, now);
    if (allowed && entry.awaiting_idr) {
      entry.idr_requested = true;
    }
    return allowed;
  }

  void frame_queue_policy_t::mark_recovery_required(
    const void *session_key,
    frame_recovery_cause_e cause
  ) {
    std::lock_guard lock {mutex_};
    auto &entry = entry_for(session_key);
    if (entry.awaiting_idr) {
      return;
    }

    entry.awaiting_idr = true;
    entry.idr_requested = false;
    entry.draining_recovery = false;
    entry.recovery_cause = cause;
  }

  void frame_queue_policy_t::erase(const void *session_key) {
    std::lock_guard lock {mutex_};
    const auto matching = std::find_if(
      entries_.begin(),
      entries_.end(),
      [session_key](const entry_t &entry) {
        return entry.active && entry.session_key == session_key;
      }
    );
    if (matching != entries_.end()) {
      *matching = {};
    }
  }

  std::size_t frame_queue_policy_t::active_sessions() const {
    std::lock_guard lock {mutex_};
    return std::count_if(
      entries_.begin(),
      entries_.end(),
      [](const entry_t &entry) {
        return entry.active;
      }
    );
  }

  frame_queue_policy_t::entry_t &frame_queue_policy_t::entry_for(
    const void *session_key
  ) {
    ++generation_;
    const auto matching = std::find_if(
      entries_.begin(),
      entries_.end(),
      [session_key](const entry_t &entry) {
        return entry.active && entry.session_key == session_key;
      }
    );
    if (matching != entries_.end()) {
      matching->last_used = generation_;
      return *matching;
    }

    auto selected = std::find_if(
      entries_.begin(),
      entries_.end(),
      [](const entry_t &entry) {
        return !entry.active;
      }
    );
    if (selected == entries_.end()) {
      selected = std::min_element(
        entries_.begin(),
        entries_.end(),
        [](const entry_t &left, const entry_t &right) {
          return left.last_used < right.last_used;
        }
      );
    }

    *selected = {
      .session_key = session_key,
      .active = true,
      .awaiting_idr = false,
      .idr_requested = false,
      .draining_recovery = false,
      .recovery_cause = frame_recovery_cause_e::none,
      .next_idr_request_at = {},
      .last_used = generation_,
    };
    return *selected;
  }

  bool frame_queue_policy_t::allow_idr_request_locked(
    entry_t &entry,
    frame_queue_time_point_t now
  ) {
    if (now < entry.next_idr_request_at) {
      return false;
    }

    entry.next_idr_request_at =
      now + minimum_idr_request_interval;
    return true;
  }

  frame_queue_policy_t &encoded_frame_queue_policy() {
    static frame_queue_policy_t policy;
    return policy;
  }

}  // namespace stream::queueing
