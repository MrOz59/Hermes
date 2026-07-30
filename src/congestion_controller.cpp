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

    constexpr std::uint64_t normal_frame_burst_multiplier = 4;
    constexpr std::uint64_t key_frame_burst_multiplier = 8;
    const auto burst_multiplier =
      is_key_frame ?
        key_frame_burst_multiplier :
        normal_frame_burst_multiplier;
    const auto scaled_burst_ceiling =
      bounded_base >
          gamestream_frame_burst_ceiling_bps / burst_multiplier ?
        gamestream_frame_burst_ceiling_bps :
        bounded_base * burst_multiplier;
    const auto burst_ceiling = std::max(
      bounded_base,
      std::min(
        scaled_burst_ceiling,
        gamestream_frame_burst_ceiling_bps
      )
    );
    const auto required_bitrate =
      gamestream_deadline_pacing_bitrate_bps(
        bounded_base,
        estimated_wire_bytes,
        nominal_send_window
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
    const auto send_window = std::max(
      nominal_send_window,
      std::min(serialization_window, bounded_maximum_window)
    );

    return {
      .pacing_bitrate_bps = pacing_bitrate,
      .send_window = send_window,
      .window_extended = send_window > nominal_send_window,
      .window_capped =
        serialization_window > bounded_maximum_window,
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
