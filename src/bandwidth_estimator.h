/**
 * @file src/bandwidth_estimator.h
 * @brief Loss/RTT estimation from feedback the GameStream client already sends.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace stream::congestion {

  using estimator_clock_t = std::chrono::steady_clock;
  using estimator_time_point_t = estimator_clock_t::time_point;

  /**
   * @brief Smoothed view of what the client reported about the path.
   *
   * `loss_ratio` counts every packet the client did not receive.
   * `unrecovered_loss_ratio` counts only the frames FEC could not repair, which
   * is what the user actually perceives. The two are deliberately separate: at
   * 5% loss fully repaired by FEC the stream looks perfect, and treating that
   * like real degradation produces exactly the constant bitrate oscillation the
   * H3 criteria forbid.
   */
  struct network_estimate_t {
    bool valid = false;
    double loss_ratio = 0.0;
    double unrecovered_loss_ratio = 0.0;
    /// Frames whose data shards all arrived, over frames observed.
    double clean_frame_ratio = 0.0;
    std::uint64_t observed_frames = 0;
    std::uint64_t observed_packets = 0;
    std::uint64_t unrecovered_frames = 0;
  };

  /**
   * @brief One frame's delivery outcome, derived from FEC feedback.
   *
   * Reed-Solomon recovers a block when at least as many shards arrive as there
   * were data shards, regardless of which ones. `recovered` therefore means the
   * client could rebuild the frame; `lost` means it could not.
   */
  enum class frame_delivery_e : std::uint8_t {
    clean,
    recovered,
    lost,
  };

  /**
   * @brief Classify one frame from the counters in a FEC feedback report.
   */
  [[nodiscard]] frame_delivery_e classify_frame_delivery(
    std::uint16_t total_data_packets,
    std::uint16_t total_repair_packets,
    std::uint16_t received_data_packets,
    std::uint16_t received_repair_packets
  ) noexcept;

  /**
   * @brief Bounded, allocation-free estimator over a sliding sample window.
   *
   * The caller supplies the monotonic time so tests and replay tools advance
   * the window deterministically. Thread safety is owned by the caller.
   */
  class bandwidth_estimator_t {
  public:
    /// Feedback older than this stops contributing to the published estimate.
    static constexpr auto sample_window = std::chrono::milliseconds {2000};
    /// Below this many frames the estimate stays invalid rather than noisy.
    static constexpr std::uint64_t minimum_frames = 8;

    void observe_frame_feedback(
      std::uint16_t total_data_packets,
      std::uint16_t total_repair_packets,
      std::uint16_t received_data_packets,
      std::uint16_t received_repair_packets,
      estimator_time_point_t now
    );

    /**
     * @brief Fold in a legacy aggregate loss report.
     *
     * These carry no per-frame structure, so they can only raise the raw loss
     * ratio. They never imply unrecovered loss on their own: without FEC
     * counters there is no way to know whether the client rebuilt the frames.
     */
    void observe_legacy_loss(
      std::uint32_t lost_packets,
      std::uint64_t packets_sent_in_interval,
      estimator_time_point_t now
    );

    void reset(estimator_time_point_t now);

    [[nodiscard]] network_estimate_t estimate(
      estimator_time_point_t now
    ) const;

  private:
    void expire(estimator_time_point_t now) const;

    struct window_t {
      std::uint64_t frames = 0;
      std::uint64_t clean_frames = 0;
      std::uint64_t unrecovered_frames = 0;
      std::uint64_t packets = 0;
      std::uint64_t lost_packets = 0;
    };

    mutable window_t current_ {};
    mutable estimator_time_point_t window_start_ {};
    mutable bool started_ = false;
  };

}  // namespace stream::congestion
