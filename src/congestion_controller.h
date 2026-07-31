/**
 * @file src/congestion_controller.h
 * @brief Injectable congestion-control boundary for Hermes media sessions.
 */
#pragma once

#include "bandwidth_estimator.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
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
    /**
     * @brief Protection for frames the rest of the stream references.
     *
     * Zero means "same as fec_ratio_ppm", which is what a controller with no
     * frame-type policy publishes.
     */
    std::uint32_t key_frame_fec_ratio_ppm = 0;
    std::uint32_t max_frame_queue_us = 0;
    std::uint32_t estimated_rtt_us = 0;
    std::uint32_t estimated_queue_delay_us = 0;
    /**
     * @brief What the path looks able to carry, in bits per second.
     *
     * Derived from unrecovered loss, which is the only capacity signal the
     * compatible feedback carries: there are no per-packet acknowledgements to
     * measure a delivery rate with. It is therefore a conservative bound on
     * the configured bitrate rather than a measurement of spare capacity, and
     * it never exceeds `encoder_bitrate_bps`.
     *
     * Advisory only. The encoder fixes its bitrate when it is configured and
     * has no runtime reconfiguration path, so nothing in the pipeline consumes
     * this value -- it is published so the adaptation can be observed against
     * real sessions before anything is wired to act on it.
     */
    std::uint64_t estimated_available_bitrate_bps = 0;
  };

  /**
   * @brief Protection level to request for one frame.
   *
   * Losing a key frame costs a black screen plus an IDR round trip, while
   * losing a normal frame costs one glitch, so the two are not worth the same
   * number of repair shards. A key-frame level below the normal one is treated
   * as unset rather than as an instruction to protect the frame less.
   */
  [[nodiscard]] constexpr std::uint32_t frame_fec_ratio_ppm(
    const congestion_target_t &target,
    bool is_key_frame
  ) noexcept {
    if (!is_key_frame) {
      return target.fec_ratio_ppm;
    }
    return target.key_frame_fec_ratio_ppm > target.fec_ratio_ppm ?
             target.key_frame_fec_ratio_ppm :
             target.fec_ratio_ppm;
  }

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

    /**
     * @brief Offer a round-trip time measured by the transport.
     *
     * Separate from feedback because it comes from the control connection's own
     * timing rather than from anything the client reports about the video
     * stream. Controllers that do not model delay ignore it.
     */
    virtual void on_rtt_sample(
      std::chrono::microseconds,
      congestion_time_point_t
    ) {
    }

    [[nodiscard]] virtual congestion_target_t target() const = 0;

    /**
     * @brief What the controller believes about the path, for diagnostics.
     *
     * A controller that measures nothing returns an invalid estimate, which is
     * what tells diagnostics to report "not adapting" rather than "healthy".
     */
    [[nodiscard]] virtual network_estimate_t estimate() const {
      return {};
    }

    /**
     * @brief Whether the controller judges the path to be queueing.
     *
     * Reported separately from the estimate because it is derived from
     * transport timing rather than from client feedback, and because it
     * explains why protection may be held while loss is visible.
     */
    [[nodiscard]] virtual bool queue_congested() const {
      return false;
    }
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

  /**
   * @brief H3 controller that adapts protection using existing client feedback.
   *
   * Deliberately narrow about what it moves. The encoder bitrate is fixed when
   * the encoder is configured and cannot be changed mid-stream, so lowering the
   * pacing rate would not slow frame production -- it would only queue frames
   * until they miss their deadline, which costs an IDR and looks far worse than
   * the original congestion. This controller therefore never lowers pacing
   * below the configured baseline and never changes what the encoder produces.
   *
   * It does maintain a conservative estimate of what the path can carry
   * (`estimated_available_bitrate_bps`), moved by the same feedback that drives
   * protection: multiplicative decrease under loss the client could not repair,
   * slow additive recovery toward the configured bitrate, and never above it.
   * That estimate is published for diagnostics only. Acting on it requires
   * encoder reconfiguration the compatible pipeline does not have, and
   * publishing it first is what makes the adaptation observable against real
   * sessions before anything is wired to apply it.
   *
   * What it does adapt is FEC: under loss the client cannot repair, more repair
   * shards directly reduce what the user sees, and pacing rises just enough to
   * carry them. Protection is raised quickly and released slowly, with a
   * deadband between the two thresholds, so a fluctuating link cannot drive
   * continuous oscillation.
   *
   * Key frames carry more protection than the level in force for normal
   * frames, from the first frame of the session rather than only after loss
   * has already been observed. That asymmetry is deliberate: a lost key frame
   * stalls the picture until a replacement is requested, encoded and delivered,
   * so it is worth spending repair shards on a frame type that is rare enough
   * for the extra bytes not to move the average bitrate.
   */
  class adaptive_congestion_controller_t final: public ICongestionController {
  public:
    /// Unrecovered-frame ratio above which protection is raised.
    static constexpr double raise_threshold = 0.02;
    /// Unrecovered-frame ratio below which protection may be released.
    static constexpr double release_threshold = 0.005;
    /// Minimum time at a given level before protection is released again.
    static constexpr auto release_hold_down = std::chrono::seconds {3};
    static constexpr std::uint32_t fec_step_up_ppm = 100'000;
    static constexpr std::uint32_t fec_step_down_ppm = 50'000;
    static constexpr std::uint32_t maximum_fec_ratio_ppm = 500'000;
    /// Protection key frames carry above the level for normal frames.
    static constexpr std::uint32_t key_frame_fec_bonus_ppm = 100'000;
    /// Ceiling for key frames, above the one that bounds normal frames.
    static constexpr std::uint32_t maximum_key_frame_fec_ratio_ppm = 600'000;
    /// Share of the estimate retained on each decrease, in per mille (85%).
    static constexpr std::uint64_t bitrate_decrease_permille = 850;
    /// Share of the configured bitrate given back per recovery step (5%).
    static constexpr std::uint64_t bitrate_recovery_step_permille = 50;
    /// The estimate never falls below this share of the configured bitrate.
    static constexpr std::uint64_t minimum_bitrate_permille = 500;
    /**
     * @brief Minimum time between two moves of the bitrate estimate.
     *
     * Longer than the FEC hold-down on purpose. Protection can follow a link
     * closely because raising it costs bandwidth that is released again within
     * seconds, while a capacity estimate that chases every burst of loss would
     * report a link far worse than the one the user is on.
     */
    static constexpr auto bitrate_hold_down = std::chrono::seconds {5};
    /**
     * @brief How long a minimum-RTT baseline is trusted.
     *
     * Queue delay is the current round trip above the path's own propagation
     * delay, and the smallest round trip seen recently is the best available
     * stand-in for that. The baseline has to expire: a route change or a queue
     * that never fully drains would otherwise be measured against a minimum
     * the path can no longer reach, and every later sample would be reported as
     * queueing that is not there.
     */
    static constexpr auto minimum_rtt_window = std::chrono::seconds {30};
    /**
     * @brief Queue delay at which the path counts as congested.
     *
     * Deliberately coarse. The sample is the transport's own smoothed round
     * trip in whole milliseconds, so a few milliseconds of movement says
     * nothing; tens of milliseconds above the path's own baseline is a queue.
     */
    static constexpr std::uint32_t queue_delay_congested_us = 30'000;
    /// Queue delay at which the path counts as drained again.
    static constexpr std::uint32_t queue_delay_drained_us = 10'000;

    /**
     * @brief Key-frame protection for a given normal-frame level.
     *
     * Never below the normal level: a host that configures protection above
     * the adaptive ceiling must not end up protecting its key frames less than
     * everything else.
     */
    [[nodiscard]] static constexpr std::uint32_t key_frame_protection_ppm(
      std::uint32_t fec_ratio_ppm
    ) noexcept {
      const auto ceiling =
        maximum_key_frame_fec_ratio_ppm > fec_ratio_ppm ?
          maximum_key_frame_fec_ratio_ppm :
          fec_ratio_ppm;
      const auto boosted =
        fec_ratio_ppm >
            std::numeric_limits<std::uint32_t>::max() -
              key_frame_fec_bonus_ppm ?
          std::numeric_limits<std::uint32_t>::max() :
          fec_ratio_ppm + key_frame_fec_bonus_ppm;
      return boosted > ceiling ? ceiling : boosted;
    }

    explicit adaptive_congestion_controller_t(
      congestion_target_t baseline
    ) noexcept;

    void on_packets_sent(const sent_packet_batch_t &batch) override;
    void on_feedback(const feedback_batch_t &feedback) override;
    void on_path_changed(const path_info_t &path) override;
    void on_rtt_sample(
      std::chrono::microseconds rtt,
      congestion_time_point_t now
    ) override;
    [[nodiscard]] congestion_target_t target() const override;

    /** @brief Current estimate, for diagnostics and tests. */
    [[nodiscard]] network_estimate_t estimate() const override;

    [[nodiscard]] bool queue_congested() const override;

  private:
    void reevaluate(estimator_time_point_t now);
    void reevaluate_available_bitrate(estimator_time_point_t now);

    mutable std::mutex mutex_;
    congestion_target_t baseline_;
    congestion_target_t current_;
    bandwidth_estimator_t estimator_;
    network_estimate_t last_estimate_ {};
    estimator_time_point_t last_raise_ {};
    estimator_time_point_t last_bitrate_change_ {};
    std::chrono::microseconds minimum_rtt_ {0};
    estimator_time_point_t minimum_rtt_observed_at_ {};
    bool queue_congested_ = false;
    std::uint64_t packets_sent_since_report_ = 0;
    bool started_ = false;
  };

  inline constexpr std::uint16_t gamestream_frame_fec_feedback_type =
    0x5502;
  inline constexpr std::size_t gamestream_legacy_loss_report_size = 32;
  inline constexpr std::size_t gamestream_frame_fec_feedback_size = 21;
  inline constexpr std::uint64_t gamestream_pacing_ceiling_bps =
    800'000'000;
  /**
   * @brief Nominal ceiling on a single frame's catch-up burst.
   *
   * Applies while the base pacing rate is below it. A base rate at or above
   * this value would otherwise have no burst headroom at all -- see
   * gamestream_frame_burst_ceiling().
   */
  inline constexpr std::uint64_t gamestream_frame_burst_ceiling_bps =
    100'000'000;

  /**
   * @brief Burst ceiling for one frame at a given base pacing rate.
   *
   * Normally the nominal ceiling applies, which keeps a single frame from
   * monopolizing the link. Once the base rate reaches that ceiling the cap
   * would sit at or below the base and silently disable catch-up entirely, so
   * the multiplier-scaled allowance takes over instead. Saturating arithmetic
   * throughout, and never above the legacy pacing ceiling.
   */
  [[nodiscard]] constexpr std::uint64_t gamestream_frame_burst_ceiling(
    std::uint64_t base_pacing_bitrate_bps,
    std::uint64_t burst_multiplier
  ) noexcept {
    const auto bounded_base =
      base_pacing_bitrate_bps > gamestream_pacing_ceiling_bps ?
        gamestream_pacing_ceiling_bps :
        base_pacing_bitrate_bps;
    const auto multiplier = burst_multiplier == 0 ? 1 : burst_multiplier;
    const auto scaled =
      bounded_base > gamestream_pacing_ceiling_bps / multiplier ?
        gamestream_pacing_ceiling_bps :
        bounded_base * multiplier;
    const auto allowance =
      bounded_base < gamestream_frame_burst_ceiling_bps ?
        (scaled > gamestream_frame_burst_ceiling_bps ?
           gamestream_frame_burst_ceiling_bps :
           scaled) :
        scaled;
    return allowance > gamestream_pacing_ceiling_bps ?
             gamestream_pacing_ceiling_bps :
             allowance;
  }

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
