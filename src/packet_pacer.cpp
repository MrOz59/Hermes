/**
 * @file src/packet_pacer.cpp
 * @brief Legacy implementation of the injectable packet pacing boundary.
 */

#include "packet_pacer.h"

#include "platform/common.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ratio>

using namespace std::literals;

namespace stream::pacing {

  legacy_packet_pacer_t::legacy_packet_pacer_t(
    IPacerTimer &timer
  ) noexcept:
      timer_ {timer},
      next_frame_start_ {timer.now()} {
  }

  void legacy_packet_pacer_t::begin_frame(
    const packet_pacing_config_t &config
  ) {
    // Preserve the legacy integer calculation exactly: 80% of 1 Gbps,
    // expressed as the number of whole packet slots in one millisecond.
    const auto packet_size_bytes =
      std::max<std::size_t>(config.packet_size_bytes, 1);
    // The quantum is a divisor in wait_before_batch()/finish_block(), and the
    // integer division above truncates to zero once a packet exceeds 100000
    // bytes. Clamp so an unusually large packet size degrades to one packet per
    // quantum instead of dividing by zero.
    packets_per_quantum_ = std::max<std::size_t>(
      std::giga::num * 80 / 100 / 1000 /
        packet_size_bytes / 8,
      1
    );
    frame_start_ = std::max(next_frame_start_, timer_.now());
    frame_packets_sent_ = 0;
    group_packets_sent_ = 0;
    packet_deadline_ = config.packet_deadline;
  }

  std::size_t legacy_packet_pacer_t::maximum_batch_packets(
    std::size_t platform_limit
  ) const noexcept {
    return platform_limit;
  }

  packet_pacing_wait_t
    legacy_packet_pacer_t::wait_before_batch() {
    if (
      group_packets_sent_ < packets_per_quantum_ &&
      frame_packets_sent_ != 0
    ) {
      if (!packet_deadline_) {
        return {};
      }
      const auto now = timer_.now();
      return {
        .deadline_expired =
          now > *packet_deadline_,
      };
    }

    const auto due =
      frame_start_ +
      std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
        frame_packets_sent_ / packets_per_quantum_;
    const auto now = timer_.now();
    if (
      packet_deadline_ &&
      (now > *packet_deadline_ ||
       due > *packet_deadline_)
    ) {
      return {
        .deadline_expired = true,
      };
    }

    auto after_wait = now;
    if (now < due) {
      timer_.sleep_for(due - now);
      after_wait = timer_.now();
    }

    group_packets_sent_ = 0;
    return {
      .waited = after_wait - now,
      .deadline_expired =
        packet_deadline_ &&
        after_wait > *packet_deadline_,
    };
  }

  void legacy_packet_pacer_t::record_batch(
    std::size_t packet_count
  ) noexcept {
    group_packets_sent_ += packet_count;
    frame_packets_sent_ += packet_count;
  }

  void legacy_packet_pacer_t::finish_block() noexcept {
    next_frame_start_ =
      frame_start_ +
      std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
        frame_packets_sent_ / packets_per_quantum_;
  }

  pacer_time_point_t
    legacy_packet_pacer_t::next_frame_start() const noexcept {
    return next_frame_start_;
  }

  rate_limited_packet_pacer_t::rate_limited_packet_pacer_t(
    IPacerTimer &timer
  ) noexcept:
      timer_ {timer},
      next_frame_start_ {timer.now()},
      next_batch_time_ {next_frame_start_} {
  }

  void rate_limited_packet_pacer_t::begin_frame(
    const packet_pacing_config_t &config
  ) {
    packet_size_bytes_ =
      std::max<std::size_t>(config.packet_size_bytes, 1);
    bitrate_bps_ =
      std::max<std::uint64_t>(config.bitrate_bps, 1);
    max_burst_duration_ =
      std::max(config.max_burst_duration, 1ns);
    packet_deadline_ = config.packet_deadline;

    next_batch_time_ =
      std::max(next_frame_start_, timer_.now());
  }

  std::size_t
    rate_limited_packet_pacer_t::maximum_batch_packets(
      std::size_t platform_limit
    ) const noexcept {
    if (platform_limit == 0) {
      return 0;
    }

    constexpr std::uint64_t bits_per_byte_nanosecond =
      8'000'000'000ULL;
    // Guard the divisor below. begin_frame() clamps the burst duration to at
    // least 1 ns, but this accessor is public and const, so it must not depend
    // on begin_frame() having run first.
    const auto burst_nanoseconds = std::max<std::uint64_t>(
      static_cast<std::uint64_t>(max_burst_duration_.count()),
      1
    );
    const auto maximum =
      std::numeric_limits<std::uint64_t>::max();
    const auto burst_rate_product =
      bitrate_bps_ > maximum / burst_nanoseconds ?
        maximum :
        bitrate_bps_ * burst_nanoseconds;
    const auto burst_bytes =
      burst_rate_product / bits_per_byte_nanosecond;
    const auto packets = static_cast<std::size_t>(
      std::min<std::uint64_t>(
        burst_bytes / packet_size_bytes_,
        std::numeric_limits<std::size_t>::max()
      )
    );

    return std::min(
      platform_limit,
      std::max<std::size_t>(packets, 1)
    );
  }

  packet_pacing_wait_t
    rate_limited_packet_pacer_t::wait_before_batch() {
    const auto before_wait = timer_.now();
    if (
      packet_deadline_ &&
      (before_wait > *packet_deadline_ ||
       next_batch_time_ > *packet_deadline_)
    ) {
      return {
        .deadline_expired = true,
      };
    }

    if (before_wait < next_batch_time_) {
      timer_.sleep_for(next_batch_time_ - before_wait);
      const auto after_wait = timer_.now();
      // Oversleep creates no pacing credit. Restarting at the actual wake
      // time prevents an immediate catch-up microburst.
      next_batch_time_ = std::max(next_batch_time_, after_wait);
      return {
        .waited = after_wait - before_wait,
        .deadline_expired =
          packet_deadline_ &&
          after_wait > *packet_deadline_,
      };
    }

    // Likewise, an idle gap cannot be accumulated as future burst credit.
    next_batch_time_ = before_wait;
    return {};
  }

  void rate_limited_packet_pacer_t::record_batch(
    std::size_t packet_count
  ) noexcept {
    next_batch_time_ += duration_for_packets(packet_count);
  }

  void rate_limited_packet_pacer_t::finish_block() noexcept {
    next_frame_start_ = next_batch_time_;
  }

  pacer_time_point_t
    rate_limited_packet_pacer_t::next_frame_start() const noexcept {
    return next_frame_start_;
  }

  std::chrono::nanoseconds
    rate_limited_packet_pacer_t::duration_for_packets(
      std::size_t packet_count
    ) const noexcept {
    const auto seconds =
      static_cast<long double>(packet_size_bytes_) *
      static_cast<long double>(packet_count) * 8.0L /
      static_cast<long double>(bitrate_bps_);
    const auto nanoseconds =
      std::ceil(seconds * 1'000'000'000.0L);
    if (
      nanoseconds >=
      static_cast<long double>(
        std::numeric_limits<std::int64_t>::max()
      )
    ) {
      return std::chrono::nanoseconds::max();
    }

    return std::chrono::nanoseconds {
      static_cast<std::int64_t>(nanoseconds)
    };
  }

  rate_limited_pacer_registry_t::rate_limited_pacer_registry_t(
    IPacerTimer &timer
  ) noexcept:
      timer_ {timer} {
  }

  IPacketPacer &rate_limited_pacer_registry_t::for_session(
    const void *session_key
  ) {
    ++generation_;
    const auto matching = std::find_if(
      entries_.begin(),
      entries_.end(),
      [session_key](const entry_t &entry) {
        return entry.session_key == session_key && entry.pacer;
      }
    );
    if (matching != entries_.end()) {
      matching->last_used = generation_;
      return *matching->pacer;
    }

    auto selected = std::find_if(
      entries_.begin(),
      entries_.end(),
      [](const entry_t &entry) {
        return !entry.pacer;
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

    selected->session_key = session_key;
    selected->pacer =
      std::make_unique<rate_limited_packet_pacer_t>(timer_);
    selected->last_used = generation_;
    return *selected->pacer;
  }

  std::size_t
    rate_limited_pacer_registry_t::active_slots() const noexcept {
    return std::count_if(
      entries_.begin(),
      entries_.end(),
      [](const entry_t &entry) {
        return static_cast<bool>(entry.pacer);
      }
    );
  }

  high_precision_pacer_timer_t::high_precision_pacer_timer_t(
    platf::high_precision_timer &timer
  ) noexcept:
      timer_ {timer} {
  }

  pacer_time_point_t high_precision_pacer_timer_t::now() const noexcept {
    return pacer_clock_t::now();
  }

  void high_precision_pacer_timer_t::sleep_for(
    const std::chrono::nanoseconds &duration
  ) {
    timer_.sleep_for(duration);
  }

}  // namespace stream::pacing
