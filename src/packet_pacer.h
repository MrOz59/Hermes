/**
 * @file src/packet_pacer.h
 * @brief Injectable packet pacing boundary for the legacy video sender.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace platf {
  struct high_precision_timer;
}

namespace stream::pacing {

  using pacer_clock_t = std::chrono::steady_clock;
  using pacer_time_point_t = pacer_clock_t::time_point;
  using pacer_duration_t = pacer_clock_t::duration;

  struct packet_pacing_config_t {
    std::size_t packet_size_bytes = 0;
    std::uint64_t bitrate_bps = 0;
    std::chrono::nanoseconds max_burst_duration =
      std::chrono::milliseconds {1};
    /// Absolute host-departure deadline. Empty disables expiry.
    std::optional<pacer_time_point_t> packet_deadline;
  };

  /**
   * @brief Result of the pacing checkpoint before one transport batch.
   *
   * deadline_expired means the caller must not submit the pending batch.
   */
  struct packet_pacing_wait_t {
    pacer_duration_t waited {};
    bool deadline_expired = false;
  };

  /**
   * @brief Monotonic clock and high-precision wait boundary used by a pacer.
   *
   * Tests can advance a fake implementation deterministically. Production
   * keeps time on steady_clock and delegates waits to the platform timer.
   */
  class IPacerTimer {
  public:
    virtual ~IPacerTimer() = default;

    [[nodiscard]] virtual pacer_time_point_t now() const noexcept = 0;
    virtual void sleep_for(const std::chrono::nanoseconds &duration) = 0;
  };

  /**
   * @brief Per-broadcaster packet pacing policy.
   *
   * A frame may contain multiple FEC blocks. Implementations must retain the
   * frame packet count across those blocks and update their carry-over
   * departure time whenever finish_block() is called.
   */
  class IPacketPacer {
  public:
    virtual ~IPacketPacer() = default;

    virtual void begin_frame(
      const packet_pacing_config_t &config
    ) = 0;
    [[nodiscard]] virtual std::size_t maximum_batch_packets(
      std::size_t platform_limit
    ) const noexcept = 0;
    virtual packet_pacing_wait_t wait_before_batch() = 0;
    virtual void record_batch(std::size_t packet_count) noexcept = 0;
    virtual void finish_block() noexcept = 0;
    [[nodiscard]] virtual pacer_time_point_t
      next_frame_start() const noexcept = 0;
  };

  /**
   * @brief Existing 80%-of-1-Gbps, 1 ms pacing behavior behind H1.
   */
  class legacy_packet_pacer_t final: public IPacketPacer {
  public:
    explicit legacy_packet_pacer_t(IPacerTimer &timer) noexcept;

    void begin_frame(
      const packet_pacing_config_t &config
    ) override;
    [[nodiscard]] std::size_t maximum_batch_packets(
      std::size_t platform_limit
    ) const noexcept override;
    packet_pacing_wait_t wait_before_batch() override;
    void record_batch(std::size_t packet_count) noexcept override;
    void finish_block() noexcept override;
    [[nodiscard]] pacer_time_point_t
      next_frame_start() const noexcept override;

  private:
    IPacerTimer &timer_;
    pacer_time_point_t next_frame_start_;
    pacer_time_point_t frame_start_;
    std::size_t packets_per_quantum_ = 0;
    std::size_t frame_packets_sent_ = 0;
    std::size_t group_packets_sent_ = 0;
    std::optional<pacer_time_point_t> packet_deadline_;
  };

  /**
   * @brief H2 leaky-bucket pacer driven by the per-session bitrate target.
   *
   * A batch contains at most max_burst_duration worth of packets. When the
   * sender wakes late, the schedule restarts from the actual wake time rather
   * than emitting back-to-back batches to catch up.
   */
  class rate_limited_packet_pacer_t final: public IPacketPacer {
  public:
    explicit rate_limited_packet_pacer_t(
      IPacerTimer &timer
    ) noexcept;

    void begin_frame(
      const packet_pacing_config_t &config
    ) override;
    [[nodiscard]] std::size_t maximum_batch_packets(
      std::size_t platform_limit
    ) const noexcept override;
    packet_pacing_wait_t wait_before_batch() override;
    void record_batch(std::size_t packet_count) noexcept override;
    void finish_block() noexcept override;
    [[nodiscard]] pacer_time_point_t
      next_frame_start() const noexcept override;

  private:
    [[nodiscard]] std::chrono::nanoseconds duration_for_packets(
      std::size_t packet_count
    ) const noexcept;

    IPacerTimer &timer_;
    pacer_time_point_t next_frame_start_;
    pacer_time_point_t next_batch_time_;
    std::size_t packet_size_bytes_ = 1;
    std::uint64_t bitrate_bps_ = 1;
    std::chrono::nanoseconds max_burst_duration_ =
      std::chrono::milliseconds {1};
    std::optional<pacer_time_point_t> packet_deadline_;
  };

  /**
   * @brief Thread-owned, bounded cache of per-session H2 pacers.
   *
   * The broadcast workers can survive session churn while another session is
   * active. Bounding this registry prevents that churn from growing transport
   * state indefinitely.
   */
  class rate_limited_pacer_registry_t {
  public:
    static constexpr std::size_t max_sessions = 32;

    explicit rate_limited_pacer_registry_t(
      IPacerTimer &timer
    ) noexcept;

    [[nodiscard]] IPacketPacer &for_session(
      const void *session_key
    );
    [[nodiscard]] std::size_t active_slots() const noexcept;

  private:
    struct entry_t {
      const void *session_key = nullptr;
      std::unique_ptr<rate_limited_packet_pacer_t> pacer;
      std::uint64_t last_used = 0;
    };

    IPacerTimer &timer_;
    std::array<entry_t, max_sessions> entries_;
    std::uint64_t generation_ = 0;
  };

  /**
   * @brief Production timer adapter for the legacy platform sleep backend.
   */
  class high_precision_pacer_timer_t final: public IPacerTimer {
  public:
    explicit high_precision_pacer_timer_t(
      platf::high_precision_timer &timer
    ) noexcept;

    [[nodiscard]] pacer_time_point_t now() const noexcept override;
    void sleep_for(const std::chrono::nanoseconds &duration) override;

  private:
    platf::high_precision_timer &timer_;
  };

}  // namespace stream::pacing
