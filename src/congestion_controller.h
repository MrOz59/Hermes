/**
 * @file src/congestion_controller.h
 * @brief Injectable congestion-control boundary for Hermes media sessions.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace stream::congestion {

  using congestion_clock_t = std::chrono::steady_clock;
  using congestion_time_point_t = congestion_clock_t::time_point;

  enum class path_address_family_e : std::uint8_t {
    ipv4,
    ipv6,
  };

  struct sent_packet_batch_t {
    std::uint32_t frame_id = 0;
    std::uint16_t first_sequence_number = 0;
    std::uint16_t packet_count = 0;
    std::uint16_t data_packet_count = 0;
    std::uint16_t repair_packet_count = 0;
    std::uint32_t wire_bytes_per_packet = 0;
    bool is_key_frame = false;
    congestion_time_point_t sent_at {};
  };

  struct legacy_loss_report_t {
    std::uint32_t lost_packets = 0;
    std::chrono::milliseconds report_interval {};
    std::uint64_t last_good_frame = 0;
  };

  struct frame_fec_feedback_t {
    std::uint32_t frame_id = 0;
    std::uint16_t highest_received_sequence_number = 0;
    std::uint16_t next_contiguous_sequence_number = 0;
    std::uint16_t missing_packets_before_highest_received = 0;
    std::uint16_t total_data_packets = 0;
    std::uint16_t total_repair_packets = 0;
    std::uint16_t received_data_packets = 0;
    std::uint16_t received_repair_packets = 0;
    std::uint8_t fec_percentage = 0;
    std::uint8_t fec_block_index = 0;
    std::uint8_t fec_block_count = 0;
  };

  /**
   * @brief Borrowed feedback view valid only for the callback duration.
   *
   * Current GameStream messages contain either one legacy aggregate or one
   * frame FEC report. A span keeps the boundary ready for native batched
   * feedback without allocating in the control thread.
   */
  struct feedback_batch_t {
    std::span<const frame_fec_feedback_t> frame_reports;
    std::optional<legacy_loss_report_t> legacy_loss;
    congestion_time_point_t received_at {};
  };

  struct path_info_t {
    path_address_family_e address_family = path_address_family_e::ipv4;
    std::uint16_t remote_port = 0;
    std::uint32_t maximum_datagram_size_bytes = 0;
    bool is_relayed = false;
    congestion_time_point_t observed_at {};
  };

  struct congestion_target_t {
    std::uint64_t encoder_bitrate_bps = 0;
    std::uint64_t pacing_bitrate_bps = 0;
    std::uint32_t fec_ratio_ppm = 0;
    std::uint32_t max_frame_queue_us = 0;
    std::uint32_t estimated_rtt_us = 0;
    std::uint32_t estimated_queue_delay_us = 0;
    std::uint32_t estimated_available_bitrate_bps = 0;
  };

  struct frame_pacing_plan_t {
    std::uint64_t pacing_bitrate_bps = 0;
    std::chrono::microseconds send_window {};
    bool window_extended = false;
    bool window_capped = false;
    bool catch_up = false;
  };

  /**
   * @brief Per-session congestion-control policy.
   *
   * Packet callbacks run on the shared video broadcaster while feedback runs
   * on the control thread and path changes run on the session video thread.
   * Implementations must be thread-safe and must not retain borrowed spans.
   */
  class ICongestionController {
  public:
    virtual ~ICongestionController() = default;

    virtual void on_packets_sent(const sent_packet_batch_t &batch) = 0;
    virtual void on_feedback(const feedback_batch_t &feedback) = 0;
    virtual void on_path_changed(const path_info_t &path) = 0;
    [[nodiscard]] virtual congestion_target_t target() const = 0;
  };

  /**
   * @brief Immutable legacy policy used until an adaptive algorithm is enabled.
   *
   * Feedback and send observations intentionally do not alter the target.
   */
  class legacy_fixed_congestion_controller_t final:
      public ICongestionController {
  public:
    explicit legacy_fixed_congestion_controller_t(
      congestion_target_t target
    ) noexcept;

    void on_packets_sent(const sent_packet_batch_t &batch) override;
    void on_feedback(const feedback_batch_t &feedback) override;
    void on_path_changed(const path_info_t &path) override;
    [[nodiscard]] congestion_target_t target() const override;

  private:
    congestion_target_t target_;
  };

  inline constexpr std::uint16_t gamestream_frame_fec_feedback_type =
    0x5502;
  inline constexpr std::size_t gamestream_legacy_loss_report_size = 32;
  inline constexpr std::size_t gamestream_frame_fec_feedback_size = 21;
  inline constexpr std::uint64_t gamestream_pacing_ceiling_bps =
    800'000'000;
  inline constexpr std::uint64_t gamestream_frame_burst_ceiling_bps =
    100'000'000;

  /**
   * @brief Derive the fixed H2 pacing rate from the configured encoder rate.
   *
   * The result reserves the configured FEC ratio plus 10% for packet headers
   * and normal encoder variation, while retaining the legacy 800 Mbps ceiling.
   */
  [[nodiscard]] std::uint64_t gamestream_fixed_pacing_bitrate_bps(
    std::uint64_t encoder_bitrate_bps,
    std::uint32_t fec_ratio_ppm
  ) noexcept;
  /**
   * @brief Raise per-frame pacing only enough to fit a bounded send window.
   *
   * The usable window reserves 15% for FEC, encryption, socket submission,
   * and timer overshoot. The result never exceeds the legacy 800 Mbps ceiling.
   */
  [[nodiscard]] std::uint64_t gamestream_deadline_pacing_bitrate_bps(
    std::uint64_t base_pacing_bitrate_bps,
    std::size_t estimated_wire_bytes,
    std::chrono::microseconds remaining_window
  ) noexcept;
  /**
   * @brief Fixed H2 encoded-frame queue budget: two frame intervals.
   */
  [[nodiscard]] std::uint32_t gamestream_fixed_frame_queue_us(
    int framerate
  ) noexcept;
  /**
   * @brief Give recovery frames extra admission time without relaxing normal
   * frame queue bounds.
   */
  [[nodiscard]] std::chrono::microseconds
    gamestream_frame_queue_budget(
      std::uint32_t base_queue_us,
      bool is_key_frame
    ) noexcept;
  /**
   * @brief Plan a bounded frame burst and a feasible host-departure window.
   *
   * Large recovery frames may use more than the average encoder bitrate, but
   * the burst is capped and the send window is extended when serialization
   * cannot fit the nominal two-frame budget. This avoids converting one late
   * IDR into another impossible IDR request.
   */
  [[nodiscard]] frame_pacing_plan_t gamestream_frame_pacing_plan(
    std::uint64_t base_pacing_bitrate_bps,
    std::size_t estimated_wire_bytes,
    std::chrono::microseconds nominal_send_window,
    std::chrono::microseconds queue_delay,
    bool is_key_frame
  ) noexcept;
  [[nodiscard]] std::optional<legacy_loss_report_t>
    parse_gamestream_legacy_loss_report(std::string_view payload) noexcept;
  [[nodiscard]] std::optional<frame_fec_feedback_t>
    parse_gamestream_frame_fec_feedback(std::string_view payload) noexcept;

}  // namespace stream::congestion
