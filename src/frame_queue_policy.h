/**
 * @file src/frame_queue_policy.h
 * @brief Bounded H2 policy for encoded-frame deadlines and recovery.
 */
#pragma once

#include "media_priority.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace stream::queueing {

  using frame_queue_clock_t = std::chrono::steady_clock;
  using frame_queue_time_point_t = frame_queue_clock_t::time_point;
  using frame_queue_duration_t = frame_queue_clock_t::duration;

  enum class frame_queue_drop_reason_e : std::uint8_t {
    none,
    deadline_expired,
    awaiting_recovery,
  };

  enum class frame_recovery_cause_e : std::uint8_t {
    none,
    deadline_expired,
    packet_deadline_expired,
    encoded_queue_overflow,
  };

  struct frame_queue_request_t {
    const void *session_key = nullptr;
    bool is_idr = false;
    std::optional<frame_queue_time_point_t> encoded_at;
    std::chrono::microseconds max_queue_time {};
    frame_queue_time_point_t now {};
  };

  struct frame_queue_decision_t {
    priority::media_priority_e priority =
      priority::media_priority_e::video_normal;
    frame_queue_drop_reason_e drop_reason =
      frame_queue_drop_reason_e::none;
    frame_recovery_cause_e recovery_cause =
      frame_recovery_cause_e::none;
    frame_queue_duration_t queue_time {};
    bool request_idr = false;
    bool idr_request_rate_limited = false;

    [[nodiscard]] constexpr bool should_send() const noexcept {
      return drop_reason == frame_queue_drop_reason_e::none;
    }
  };

  /**
   * @brief Thread-safe per-session queue policy.
   *
   * Dropping an inter frame makes later dependent frames unsafe. The policy
   * therefore requests one IDR and rejects dependent frames until a fresh IDR
   * arrives. State is LRU-bounded so session churn cannot grow it forever.
   */
  class frame_queue_policy_t {
  public:
    static constexpr std::size_t max_sessions = 32;
    static constexpr auto minimum_idr_request_interval =
      std::chrono::milliseconds {250};

    [[nodiscard]] frame_queue_decision_t evaluate(
      const frame_queue_request_t &request
    );
    /**
     * @brief Gate an IDR event through the shared per-session cooldown.
     */
    [[nodiscard]] bool allow_idr_request(
      const void *session_key,
      frame_queue_time_point_t now = frame_queue_clock_t::now()
    );
    void mark_recovery_required(
      const void *session_key,
      frame_recovery_cause_e cause =
        frame_recovery_cause_e::encoded_queue_overflow
    );
    void erase(const void *session_key);
    [[nodiscard]] std::size_t active_sessions() const;

  private:
    struct entry_t {
      const void *session_key = nullptr;
      bool active = false;
      bool awaiting_idr = false;
      bool idr_requested = false;
      frame_recovery_cause_e recovery_cause =
        frame_recovery_cause_e::none;
      frame_queue_time_point_t next_idr_request_at {};
      std::uint64_t last_used = 0;
    };

    entry_t &entry_for(const void *session_key);
    static bool allow_idr_request_locked(
      entry_t &entry,
      frame_queue_time_point_t now
    );

    std::array<entry_t, max_sessions> entries_;
    std::uint64_t generation_ = 0;
    mutable std::mutex mutex_;
  };

  /**
   * @brief Process-wide policy shared by encoder overflow and broadcaster.
   */
  frame_queue_policy_t &encoded_frame_queue_policy();

  /**
   * @brief RTP sequence numbers consumed by one emitted FEC block.
   *
   * Every shard is stamped with its sequence number (and streamPacketIndex)
   * before any datagram is sent, so all of them are spent even when the send
   * loop aborts early on a deadline. Advancing by only the transmitted count
   * would reuse the remaining numbers for different payload in the next frame,
   * making the client's sequence numbers appear to move backwards -- which
   * breaks RtpVideoQueue's contiguity assumption, its duplicate detection, and
   * FEC recovery.
   *
   * @param stamped_shards Shards that received a sequence number.
   * @param sent_shards Shards actually submitted to the transport.
   */
  [[nodiscard]] constexpr std::uint32_t sequence_numbers_consumed(
    std::size_t stamped_shards,
    std::size_t sent_shards
  ) noexcept {
    static_cast<void>(sent_shards);
    return static_cast<std::uint32_t>(stamped_shards);
  }

}  // namespace stream::queueing
