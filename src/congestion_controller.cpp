/**
 * @file src/congestion_controller.cpp
 * @brief Legacy implementation and GameStream adapters for congestion control.
 */

#include "congestion_controller.h"

#include "utility.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace stream::congestion {
  namespace {

    template<typename T>
    T read_little(std::string_view payload, std::size_t offset) noexcept {
      T value {};
      std::memcpy(&value, payload.data() + offset, sizeof(value));
      return util::endian::little(value);
    }

    template<typename T>
    T read_big(std::string_view payload, std::size_t offset) noexcept {
      T value {};
      std::memcpy(&value, payload.data() + offset, sizeof(value));
      return util::endian::big(value);
    }

  }  // namespace

  legacy_fixed_congestion_controller_t::
    legacy_fixed_congestion_controller_t(
      congestion_target_t target
    ) noexcept:
      target_ {target} {
  }

  void legacy_fixed_congestion_controller_t::on_packets_sent(
    const sent_packet_batch_t &
  ) {
  }

  void legacy_fixed_congestion_controller_t::on_feedback(
    const feedback_batch_t &
  ) {
  }

  void legacy_fixed_congestion_controller_t::on_path_changed(
    const path_info_t &
  ) {
  }

  congestion_target_t
    legacy_fixed_congestion_controller_t::target() const {
    return target_;
  }

  adaptive_congestion_controller_t::adaptive_congestion_controller_t(
    congestion_target_t baseline
  ) noexcept:
      baseline_ {baseline},
      current_ {baseline} {
    // Key frames are protected from the first frame of the session, before any
    // feedback exists. The very first IDR is the one frame whose loss the user
    // always notices, and by the time loss has been measured it is long gone.
    baseline_.key_frame_fec_ratio_ppm =
      key_frame_protection_ppm(baseline_.fec_ratio_ppm);
    current_.key_frame_fec_ratio_ppm = baseline_.key_frame_fec_ratio_ppm;
    // Until feedback says otherwise the path is assumed to carry what the
    // encoder was configured for, so a session that never sees loss reports
    // the configured bitrate rather than zero.
    baseline_.estimated_available_bitrate_bps = baseline_.encoder_bitrate_bps;
    current_.estimated_available_bitrate_bps =
      baseline_.estimated_available_bitrate_bps;
  }

  void adaptive_congestion_controller_t::on_packets_sent(
    const sent_packet_batch_t &batch
  ) {
    std::lock_guard lock {mutex_};
    // Legacy loss reports carry a count, not a ratio. Tracking what was sent
    // between reports is the only way to turn one into the other.
    packets_sent_since_report_ += batch.packet_count;
  }

  void adaptive_congestion_controller_t::on_feedback(
    const feedback_batch_t &feedback
  ) {
    std::lock_guard lock {mutex_};
    const auto now = feedback.received_at;
    if (!started_) {
      estimator_.reset(now);
      last_raise_ = now;
      last_bitrate_change_ = now;
      started_ = true;
    }

    for (const auto &report : feedback.frame_reports) {
      estimator_.observe_frame_feedback(
        report.total_data_packets,
        report.total_repair_packets,
        report.received_data_packets,
        report.received_repair_packets,
        now
      );
    }

    if (feedback.legacy_loss) {
      estimator_.observe_legacy_loss(
        feedback.legacy_loss->lost_packets,
        packets_sent_since_report_,
        now
      );
      packets_sent_since_report_ = 0;
    }

    reevaluate(now);
  }

  void adaptive_congestion_controller_t::on_path_changed(
    const path_info_t &path
  ) {
    std::lock_guard lock {mutex_};
    // A new path invalidates everything measured about the old one. Returning
    // to baseline is safer than carrying protection tuned for a link that no
    // longer exists.
    current_ = baseline_;
    estimator_.reset(path.observed_at);
    last_estimate_ = {};
    last_raise_ = path.observed_at;
    last_bitrate_change_ = path.observed_at;
    minimum_rtt_ = std::chrono::microseconds {0};
    minimum_rtt_observed_at_ = path.observed_at;
    packets_sent_since_report_ = 0;
    started_ = true;
  }

  void adaptive_congestion_controller_t::on_rtt_sample(
    std::chrono::microseconds rtt,
    congestion_time_point_t now
  ) {
    if (rtt <= std::chrono::microseconds {0}) {
      // The transport has not measured anything yet.
      return;
    }

    std::lock_guard lock {mutex_};
    // Re-baseline when this sample is lower than the current minimum, and also
    // when the minimum has simply grown old: an expired baseline is what keeps
    // a path whose propagation delay changed from reporting permanent queueing.
    if (minimum_rtt_ <= std::chrono::microseconds {0} ||
        rtt < minimum_rtt_ ||
        now - minimum_rtt_observed_at_ > minimum_rtt_window) {
      minimum_rtt_ = rtt;
      minimum_rtt_observed_at_ = now;
    }

    current_.estimated_rtt_us = static_cast<std::uint32_t>(rtt.count());
    current_.estimated_queue_delay_us =
      static_cast<std::uint32_t>((rtt - minimum_rtt_).count());
  }

  congestion_target_t adaptive_congestion_controller_t::target() const {
    std::lock_guard lock {mutex_};
    return current_;
  }

  network_estimate_t adaptive_congestion_controller_t::estimate() const {
    std::lock_guard lock {mutex_};
    return last_estimate_;
  }

  void adaptive_congestion_controller_t::reevaluate(
    estimator_time_point_t now
  ) {
    const auto estimate = estimator_.estimate(now);
    last_estimate_ = estimate;

    if (!estimate.valid) {
      // No signal is not evidence of a healthy path. The sample window empties
      // whenever feedback pauses, which happens routinely between report
      // bursts, so resetting here would erase the adaptation every few seconds
      // and re-derive it from scratch. The last conclusion stands until new
      // feedback moves it; the session starts at the configured bitrate and a
      // path change puts it back there.
      return;
    }

    reevaluate_available_bitrate(estimate, now);

    // A host may configure protection above the adaptive ceiling. The ceiling
    // must then yield: raising must never end up lowering what was configured.
    const auto ceiling_ppm =
      std::max(maximum_fec_ratio_ppm, baseline_.fec_ratio_ppm);

    auto fec_ratio_ppm = current_.fec_ratio_ppm;
    if (estimate.unrecovered_loss_ratio > raise_threshold) {
      fec_ratio_ppm = std::min(
        fec_ratio_ppm + fec_step_up_ppm,
        ceiling_ppm
      );
      last_raise_ = now;
    } else if (
      estimate.unrecovered_loss_ratio < release_threshold &&
      now - last_raise_ >= release_hold_down
    ) {
      // Release one step at a time, never below what the host configured.
      fec_ratio_ppm =
        fec_ratio_ppm > baseline_.fec_ratio_ppm + fec_step_down_ppm ?
          fec_ratio_ppm - fec_step_down_ppm :
          baseline_.fec_ratio_ppm;
      last_raise_ = now;
    }
    // Between the thresholds the level is held: that deadband is what keeps a
    // link hovering near one threshold from oscillating every window.

    current_.fec_ratio_ppm = fec_ratio_ppm;
    current_.key_frame_fec_ratio_ppm =
      key_frame_protection_ppm(fec_ratio_ppm);
    // Pacing must carry the extra repair shards, and never drops below the
    // configured baseline: the encoder keeps producing at its fixed rate, so a
    // lower pacing rate would build queue instead of relieving congestion.
    current_.pacing_bitrate_bps = std::max(
      baseline_.pacing_bitrate_bps,
      gamestream_fixed_pacing_bitrate_bps(
        baseline_.encoder_bitrate_bps,
        fec_ratio_ppm
      )
    );
  }

  void adaptive_congestion_controller_t::reevaluate_available_bitrate(
    const network_estimate_t &estimate,
    estimator_time_point_t now
  ) {
    const auto configured = baseline_.encoder_bitrate_bps;
    if (configured == 0) {
      // Nothing to scale against, so there is nothing meaningful to report.
      return;
    }

    auto available = current_.estimated_available_bitrate_bps;
    if (available == 0 || available > configured) {
      available = configured;
    }

    // One move per hold-down window at most. The estimate is already smoothed
    // over the sample window; reacting again before the previous move could
    // show up in the feedback would compound decreases on stale evidence.
    if (now - last_bitrate_change_ < bitrate_hold_down) {
      current_.estimated_available_bitrate_bps = available;
      return;
    }

    const auto floor_bps = std::max<std::uint64_t>(
      configured * minimum_bitrate_permille / 1000,
      1
    );

    if (estimate.unrecovered_loss_ratio > raise_threshold) {
      // Multiplicative decrease: loss the client could not repair means the
      // path is already past what it can carry, and backing off in proportion
      // is what converges quickly enough to matter.
      available = std::max(
        available * bitrate_decrease_permille / 1000,
        floor_bps
      );
      last_bitrate_change_ = now;
    } else if (estimate.unrecovered_loss_ratio < release_threshold) {
      // Additive recovery in fixed steps of the configured bitrate, so a link
      // that just recovered is probed gently instead of being handed back
      // everything it had just failed to carry.
      const auto step = std::max<std::uint64_t>(
        configured * bitrate_recovery_step_permille / 1000,
        1
      );
      available = std::min(
        available > configured - step ? configured : available + step,
        configured
      );
      last_bitrate_change_ = now;
    }
    // Between the thresholds the estimate is held, for the same reason
    // protection is: a deadband is what keeps a link hovering near one
    // threshold from oscillating every window.

    current_.estimated_available_bitrate_bps = available;
  }

  std::uint64_t gamestream_fixed_pacing_bitrate_bps(
    std::uint64_t encoder_bitrate_bps,
    std::uint32_t fec_ratio_ppm
  ) noexcept {
    if (encoder_bitrate_bps == 0) {
      return gamestream_pacing_ceiling_bps;
    }

    constexpr long double ppm = 1'000'000.0L;
    constexpr long double header_and_variation_headroom = 1.10L;
    constexpr std::uint64_t minimum_pacing_bitrate_bps = 1'000'000;
    const auto bounded_fec_ratio =
      std::min<std::uint32_t>(fec_ratio_ppm, 1'000'000);
    const auto pacing_bitrate =
      static_cast<long double>(encoder_bitrate_bps) *
      (1.0L + static_cast<long double>(bounded_fec_ratio) / ppm) *
      header_and_variation_headroom;

    return static_cast<std::uint64_t>(std::clamp<long double>(
      pacing_bitrate,
      minimum_pacing_bitrate_bps,
      gamestream_pacing_ceiling_bps
    ));
  }

  std::uint64_t gamestream_deadline_pacing_bitrate_bps(
    std::uint64_t base_pacing_bitrate_bps,
    std::size_t estimated_wire_bytes,
    std::chrono::microseconds remaining_window
  ) noexcept {
    const auto bounded_base = std::min(
      base_pacing_bitrate_bps,
      gamestream_pacing_ceiling_bps
    );
    if (
      estimated_wire_bytes == 0 ||
      remaining_window <= std::chrono::microseconds::zero()
    ) {
      return bounded_base;
    }

    constexpr long double bits_per_byte = 8.0L;
    constexpr long double microseconds_per_second = 1'000'000.0L;
    constexpr long double usable_window_fraction = 0.85L;
    const auto required_bitrate =
      std::ceil(
        static_cast<long double>(estimated_wire_bytes) *
        bits_per_byte *
        microseconds_per_second /
        (
          static_cast<long double>(remaining_window.count()) *
          usable_window_fraction
        )
      );

    return static_cast<std::uint64_t>(
      std::clamp<long double>(
        required_bitrate,
        static_cast<long double>(bounded_base),
        static_cast<long double>(gamestream_pacing_ceiling_bps)
      )
    );
  }

  std::uint32_t gamestream_fixed_frame_queue_us(
    int framerate
  ) noexcept {
    constexpr std::uint32_t minimum_queue_us = 8'000;
    constexpr std::uint32_t maximum_queue_us = 100'000;
    constexpr std::uint32_t fallback_queue_us = 50'000;
    if (framerate <= 0) {
      return fallback_queue_us;
    }

    const auto frame_interval_us =
      (1'000'000ULL + static_cast<std::uint64_t>(framerate) - 1) /
      static_cast<std::uint64_t>(framerate);
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
      frame_interval_us * 2,
      minimum_queue_us,
      maximum_queue_us
    ));
  }

  std::chrono::microseconds gamestream_frame_queue_budget(
    std::uint32_t base_queue_us,
    bool is_key_frame
  ) noexcept {
    const auto base =
      std::chrono::microseconds {base_queue_us};
    if (!is_key_frame || base_queue_us == 0) {
      return base;
    }

    constexpr auto maximum_key_frame_queue =
      std::chrono::milliseconds {100};
    return std::min(
      base * 3,
      std::chrono::duration_cast<std::chrono::microseconds>(
        maximum_key_frame_queue
      )
    );
  }

  frame_pacing_plan_t gamestream_frame_pacing_plan(
    std::uint64_t base_pacing_bitrate_bps,
    std::size_t estimated_wire_bytes,
    std::chrono::microseconds nominal_send_window,
    std::chrono::microseconds queue_delay,
    bool is_key_frame
  ) noexcept {
    const auto bounded_base = std::min(
      base_pacing_bitrate_bps,
      gamestream_pacing_ceiling_bps
    );
    if (
      estimated_wire_bytes == 0 ||
      nominal_send_window <= std::chrono::microseconds::zero()
    ) {
      return {
        .pacing_bitrate_bps = bounded_base,
        .send_window = nominal_send_window,
      };
    }

    auto target_send_window = nominal_send_window;
    if (!is_key_frame && queue_delay > std::chrono::microseconds::zero()) {
      target_send_window =
        queue_delay >= nominal_send_window ?
          std::chrono::microseconds {1} :
          nominal_send_window - queue_delay;
    }

    constexpr std::uint64_t normal_frame_burst_multiplier = 4;
    constexpr std::uint64_t key_frame_burst_multiplier = 8;
    const auto burst_multiplier =
      is_key_frame ?
        key_frame_burst_multiplier :
        normal_frame_burst_multiplier;
    // Scales with the base rate, so catch-up keeps working above the absolute
    // burst ceiling instead of collapsing to the base rate and disabling
    // itself for high-bitrate sessions.
    const auto burst_ceiling = std::max(
      bounded_base,
      gamestream_frame_burst_ceiling(bounded_base, burst_multiplier)
    );
    const auto required_bitrate =
      gamestream_deadline_pacing_bitrate_bps(
        bounded_base,
        estimated_wire_bytes,
        target_send_window
      );
    const auto pacing_bitrate =
      std::min(required_bitrate, burst_ceiling);

    constexpr long double bits_per_byte = 8.0L;
    constexpr long double microseconds_per_second = 1'000'000.0L;
    constexpr long double usable_window_fraction = 0.85L;
    const auto serialization_window_value = std::ceil(
      static_cast<long double>(estimated_wire_bytes) *
      bits_per_byte *
      microseconds_per_second /
      (
        static_cast<long double>(
          std::max<std::uint64_t>(pacing_bitrate, 1)
        ) *
        usable_window_fraction
      )
    );
    const auto serialization_window =
      std::chrono::microseconds {
        static_cast<std::chrono::microseconds::rep>(
          std::min<long double>(
            serialization_window_value,
            static_cast<long double>(
              std::numeric_limits<
                std::chrono::microseconds::rep
              >::max()
            )
          )
        )
      };
    const auto maximum_send_window =
      is_key_frame ?
        std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::milliseconds {250}
        ) :
        std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::milliseconds {100}
        );
    const auto bounded_maximum_window =
      std::max(nominal_send_window, maximum_send_window);
    // Queue age makes a late normal frame use a higher pacing rate, but it
    // must not also collapse the packet deadline to the raw serialization
    // estimate. FEC generation, encryption, timer overshoot, and socket
    // submission still need bounded headroom. The higher rate drains the
    // backlog; the nominal window remains the minimum departure deadline.
    const auto minimum_send_window = nominal_send_window;
    const auto send_window = std::max(
      minimum_send_window,
      std::min(serialization_window, bounded_maximum_window)
    );

    return {
      .pacing_bitrate_bps = pacing_bitrate,
      .send_window = send_window,
      .window_extended = send_window > nominal_send_window,
      .window_capped =
        serialization_window > bounded_maximum_window,
      .catch_up =
        !is_key_frame &&
        queue_delay > std::chrono::microseconds::zero() &&
        pacing_bitrate > bounded_base,
    };
  }

  std::optional<legacy_loss_report_t>
    parse_gamestream_legacy_loss_report(
      std::string_view payload
    ) noexcept {
    if (payload.size() < gamestream_legacy_loss_report_size) {
      return std::nullopt;
    }

    return legacy_loss_report_t {
      .lost_packets = read_little<std::uint32_t>(payload, 0),
      .report_interval = std::chrono::milliseconds {
        read_little<std::int32_t>(payload, 4)
      },
      .last_good_frame = read_little<std::uint64_t>(payload, 12),
    };
  }

  std::optional<frame_fec_feedback_t>
    parse_gamestream_frame_fec_feedback(
      std::string_view payload
    ) noexcept {
    if (payload.size() < gamestream_frame_fec_feedback_size) {
      return std::nullopt;
    }

    return frame_fec_feedback_t {
      .frame_id = read_big<std::uint32_t>(payload, 0),
      .highest_received_sequence_number =
        read_big<std::uint16_t>(payload, 4),
      .next_contiguous_sequence_number =
        read_big<std::uint16_t>(payload, 6),
      .missing_packets_before_highest_received =
        read_big<std::uint16_t>(payload, 8),
      .total_data_packets = read_big<std::uint16_t>(payload, 10),
      .total_repair_packets = read_big<std::uint16_t>(payload, 12),
      .received_data_packets = read_big<std::uint16_t>(payload, 14),
      .received_repair_packets = read_big<std::uint16_t>(payload, 16),
      .fec_percentage =
        static_cast<std::uint8_t>(payload[18]),
      .fec_block_index =
        static_cast<std::uint8_t>(payload[19]),
      .fec_block_count =
        static_cast<std::uint8_t>(payload[20]),
    };
  }

}  // namespace stream::congestion
