/**
 * @file src/bandwidth_estimator.cpp
 * @brief Loss estimation over GameStream's existing client feedback.
 */

#include "bandwidth_estimator.h"

#include <algorithm>

namespace stream::congestion {

  frame_delivery_e classify_frame_delivery(
    std::uint16_t total_data_packets,
    std::uint16_t total_repair_packets,
    std::uint16_t received_data_packets,
    std::uint16_t received_repair_packets
  ) noexcept {
    if (total_data_packets == 0) {
      return frame_delivery_e::clean;
    }

    // A client cannot receive more than was sent; clamp rather than trust the
    // peer, since these counters come off the wire.
    const auto received_data =
      std::min(received_data_packets, total_data_packets);
    const auto received_repair =
      std::min(received_repair_packets, total_repair_packets);

    if (received_data == total_data_packets) {
      return frame_delivery_e::clean;
    }

    // Reed-Solomon rebuilds the block from any total_data_packets shards.
    const auto received_total =
      static_cast<std::uint32_t>(received_data) +
      static_cast<std::uint32_t>(received_repair);
    return received_total >= total_data_packets ?
             frame_delivery_e::recovered :
             frame_delivery_e::lost;
  }

  void bandwidth_estimator_t::observe_frame_feedback(
    std::uint16_t total_data_packets,
    std::uint16_t total_repair_packets,
    std::uint16_t received_data_packets,
    std::uint16_t received_repair_packets,
    estimator_time_point_t now
  ) {
    expire(now);

    const auto delivery = classify_frame_delivery(
      total_data_packets,
      total_repair_packets,
      received_data_packets,
      received_repair_packets
    );

    const auto sent = static_cast<std::uint64_t>(total_data_packets) +
                      static_cast<std::uint64_t>(total_repair_packets);
    const auto received =
      std::min<std::uint64_t>(
        static_cast<std::uint64_t>(received_data_packets) +
          static_cast<std::uint64_t>(received_repair_packets),
        sent
      );

    ++current_.frames;
    current_.packets += sent;
    current_.lost_packets += sent - received;
    if (delivery == frame_delivery_e::clean) {
      ++current_.clean_frames;
    } else if (delivery == frame_delivery_e::lost) {
      ++current_.unrecovered_frames;
    }
  }

  void bandwidth_estimator_t::observe_legacy_loss(
    std::uint32_t lost_packets,
    std::uint64_t packets_sent_in_interval,
    estimator_time_point_t now
  ) {
    expire(now);

    if (packets_sent_in_interval == 0) {
      return;
    }

    current_.packets += packets_sent_in_interval;
    current_.lost_packets += std::min<std::uint64_t>(
      lost_packets,
      packets_sent_in_interval
    );
  }

  void bandwidth_estimator_t::reset(estimator_time_point_t now) {
    current_ = {};
    window_start_ = now;
    started_ = true;
  }

  network_estimate_t bandwidth_estimator_t::estimate(
    estimator_time_point_t now
  ) const {
    expire(now);

    if (current_.frames < minimum_frames) {
      // Too few samples to distinguish a real trend from one unlucky frame.
      return {};
    }

    const auto frames = static_cast<double>(current_.frames);
    const auto packets = static_cast<double>(current_.packets);
    return {
      .valid = true,
      .loss_ratio = packets > 0.0 ?
                      static_cast<double>(current_.lost_packets) / packets :
                      0.0,
      .unrecovered_loss_ratio =
        static_cast<double>(current_.unrecovered_frames) / frames,
      .clean_frame_ratio =
        static_cast<double>(current_.clean_frames) / frames,
      .observed_frames = current_.frames,
      .observed_packets = current_.packets,
      .unrecovered_frames = current_.unrecovered_frames,
    };
  }

  void bandwidth_estimator_t::expire(estimator_time_point_t now) const {
    if (!started_) {
      window_start_ = now;
      started_ = true;
      return;
    }

    if (now - window_start_ < sample_window) {
      return;
    }

    // Tumbling window: a fresh estimate must not stay anchored to conditions
    // that already ended, or recovery after a bad patch would be invisible.
    current_ = {};
    window_start_ = now;
  }

}  // namespace stream::congestion
