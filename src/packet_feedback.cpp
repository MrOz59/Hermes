/**
 * @file src/packet_feedback.cpp
 * @brief Parsing and analysis of per-packet feedback reports.
 */

#include "packet_feedback.h"

#include "utility.h"

#include <algorithm>
#include <cstring>

namespace stream::congestion {
  namespace {

    template<typename T>
    T read_big(std::string_view payload, std::size_t offset) noexcept {
      T value {};
      std::memcpy(&value, payload.data() + offset, sizeof(value));
      return util::endian::big(value);
    }

    constexpr std::uint16_t received_flag = 0x8000;
    constexpr std::uint16_t arrival_offset_mask = 0x7fff;

  }  // namespace

  std::optional<packet_feedback_report_t> parse_packet_feedback(
    std::string_view payload,
    std::span<packet_metric_t> storage
  ) noexcept {
    if (payload.size() < packet_feedback_header_size) {
      return std::nullopt;
    }

    const auto report_sequence = read_big<std::uint16_t>(payload, 0);
    const auto base_sequence = read_big<std::uint16_t>(payload, 2);
    const auto packet_count = read_big<std::uint16_t>(payload, 4);
    const auto reference_time_us = read_big<std::uint32_t>(payload, 6);

    if (packet_count == 0 ||
        packet_count > maximum_packet_feedback_metrics ||
        packet_count > storage.size()) {
      return std::nullopt;
    }

    // The declared count has to match the payload exactly. Trusting the count
    // alone would read whatever follows a truncated message.
    const auto expected_size =
      packet_feedback_header_size +
      static_cast<std::size_t>(packet_count) * packet_feedback_metric_size;
    if (payload.size() != expected_size) {
      return std::nullopt;
    }

    for (std::uint16_t index = 0; index < packet_count; ++index) {
      const auto metric = read_big<std::uint16_t>(
        payload,
        packet_feedback_header_size +
          static_cast<std::size_t>(index) * packet_feedback_metric_size
      );
      const bool received = (metric & received_flag) != 0;
      storage[index] = {
        // Sequence numbers wrap, which is exactly what the addition does here.
        .sequence_number =
          static_cast<std::uint16_t>(base_sequence + index),
        .received = received,
        .arrival_offset = std::chrono::microseconds {
          received ?
            static_cast<std::uint32_t>(metric & arrival_offset_mask) *
              packet_feedback_time_unit_us :
            0
        },
      };
    }

    return packet_feedback_report_t {
      .report_sequence = report_sequence,
      .base_sequence = base_sequence,
      .reference_time = std::chrono::microseconds {reference_time_us},
      .metrics = storage.first(packet_count),
    };
  }

  void packet_send_history_t::record(
    std::uint16_t first_sequence_number,
    std::uint16_t packet_count,
    std::uint32_t wire_bytes_per_packet,
    feedback_time_point_t sent_at
  ) {
    for (std::uint16_t index = 0; index < packet_count; ++index) {
      const auto sequence_number =
        static_cast<std::uint16_t>(first_sequence_number + index);
      auto &slot = slots_[sequence_number % capacity];
      slot = {
        .sequence_number = sequence_number,
        .occupied = true,
        .wire_bytes = wire_bytes_per_packet,
        // Every packet of a batch shares the batch's departure time. The pacer
        // emits them as one burst, so a finer timestamp would be invented
        // precision rather than measurement.
        .sent_at = sent_at,
      };
    }
  }

  packet_send_history_t::record_t packet_send_history_t::find(
    std::uint16_t sequence_number
  ) const {
    const auto &slot = slots_[sequence_number % capacity];
    if (!slot.occupied || slot.sequence_number != sequence_number) {
      return {};
    }
    return {
      .known = true,
      .wire_bytes = slot.wire_bytes,
      .sent_at = slot.sent_at,
    };
  }

  void packet_send_history_t::reset() {
    slots_ = {};
  }

  delivery_estimate_t analyze_packet_feedback(
    const packet_feedback_report_t &report,
    const packet_send_history_t &history
  ) {
    delivery_estimate_t estimate;

    bool have_first = false;
    std::chrono::microseconds first_arrival {0};
    std::chrono::microseconds last_arrival {0};
    feedback_time_point_t first_departure {};
    feedback_time_point_t last_departure {};
    std::uint64_t delivered_bytes = 0;
    std::uint64_t first_delivered_bytes = 0;

    for (const auto &metric : report.metrics) {
      if (!metric.received) {
        ++estimate.lost_packets;
        continue;
      }

      const auto sent = history.find(metric.sequence_number);
      if (!sent.known) {
        // Acknowledged, but its record has already been overwritten. Counting
        // it as delivered without its size or departure would distort both the
        // rate and the gradient.
        continue;
      }

      ++estimate.delivered_packets;
      delivered_bytes += sent.wire_bytes;

      if (!have_first) {
        first_arrival = metric.arrival_offset;
        first_departure = sent.sent_at;
        first_delivered_bytes = sent.wire_bytes;
        have_first = true;
      }
      last_arrival = metric.arrival_offset;
      last_departure = sent.sent_at;
    }

    // One packet establishes no interval, so neither a rate nor a gradient can
    // be computed from it.
    if (estimate.delivered_packets < 2) {
      return estimate;
    }

    const auto arrival_span = last_arrival - first_arrival;
    const auto departure_span =
      std::chrono::duration_cast<std::chrono::microseconds>(
        last_departure - first_departure
      );

    // Delay gradient: how much the arrival span stretched relative to the
    // departure span. A queue filling stretches it; a queue draining
    // compresses it. Independent of any clock offset between the two ends.
    estimate.delay_gradient_us =
      arrival_span.count() - departure_span.count();

    if (arrival_span.count() > 0) {
      // The first delivered packet's bytes are excluded on purpose: they were
      // in flight before the interval started, so counting them would inflate
      // the rate on short reports.
      const auto measured_bytes = delivered_bytes - first_delivered_bytes;
      estimate.delivery_rate_bps = static_cast<std::uint64_t>(
        static_cast<long double>(measured_bytes) * 8.0L * 1'000'000.0L /
        static_cast<long double>(arrival_span.count())
      );
      estimate.valid = true;
    }

    return estimate;
  }

}  // namespace stream::congestion
