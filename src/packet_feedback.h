/**
 * @file src/packet_feedback.h
 * @brief Per-packet feedback and the delivery/delay estimates derived from it.
 *
 * The compatible GameStream feedback reports what a frame's FEC could and
 * could not repair. That is enough to see damage, but not enough to measure
 * capacity: without knowing which packets arrived and when, a host can only
 * infer congestion after it has already hurt the picture.
 *
 * The `packet_feedback` extension closes that gap with a report modelled on
 * RFC 8888 (RTCP Congestion Control Feedback): a run of sequence numbers, each
 * marked received or lost, and each received one carrying the moment it
 * arrived. Two things follow from it that nothing else in the pipeline can
 * provide -- the rate the path actually delivered, and whether one-way delay
 * is trending up, which is congestion becoming visible before any loss.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace stream::congestion {

  using feedback_clock_t = std::chrono::steady_clock;
  using feedback_time_point_t = feedback_clock_t::time_point;

  /**
   * @brief Wire format of `packet_feedback` version 1.
   *
   * @verbatim
   * offset size field
   * 0      2    report sequence, increments per report
   * 2      2    base sequence, first packet covered
   * 4      2    packet count covered by this report
   * 6      4    reference time, receiver clock in microseconds
   * 10     2*n  one metric per covered packet, in sequence order:
   *               bit 15    received
   *               bits 0-14 arrival offset from the reference time,
   *                         in units of 64 us
   * @endverbatim
   *
   * All fields are big endian, matching the existing control messages. The
   * offset unit gives a report a span of roughly two seconds, which is far
   * longer than any report worth acting on, and keeps a metric at two bytes.
   *
   * The receiver clock is never compared against the host clock: only
   * differences between arrival times within a report are used, so the two
   * clocks do not need to agree on anything but their rate.
   */
  inline constexpr std::uint16_t hermes_packet_feedback_type = 0x5601;
  inline constexpr std::size_t packet_feedback_header_size = 10;
  inline constexpr std::size_t packet_feedback_metric_size = 2;
  /// Arrival offsets are carried in units of this many microseconds.
  inline constexpr std::uint32_t packet_feedback_time_unit_us = 64;
  /// A single report never covers more than this many packets.
  inline constexpr std::size_t maximum_packet_feedback_metrics = 512;

  struct packet_metric_t {
    std::uint16_t sequence_number = 0;
    bool received = false;
    /// Arrival, relative to the report's reference time. Zero when lost.
    std::chrono::microseconds arrival_offset {0};
  };

  /**
   * @brief One parsed report.
   *
   * Metrics are borrowed from the caller's storage so the control thread can
   * parse without allocating.
   */
  struct packet_feedback_report_t {
    std::uint16_t report_sequence = 0;
    std::uint16_t base_sequence = 0;
    std::chrono::microseconds reference_time {0};
    std::span<const packet_metric_t> metrics;
  };

  /**
   * @brief Parse a `packet_feedback` v1 payload.
   *
   * Rejects a payload whose declared count does not match its length, rather
   * than reading what happens to follow it.
   */
  [[nodiscard]] std::optional<packet_feedback_report_t> parse_packet_feedback(
    std::string_view payload,
    std::span<packet_metric_t> storage
  ) noexcept;

  /**
   * @brief What a report says about the path.
   *
   * `valid` is false when the report covered too little to conclude anything,
   * which is the normal case for the first reports of a session.
   */
  struct delivery_estimate_t {
    bool valid = false;
    /// Bytes delivered per second, measured from arrival times.
    std::uint64_t delivery_rate_bps = 0;
    /**
     * @brief Change in one-way delay across the report, in microseconds.
     *
     * Positive means packets are arriving further apart than they were sent,
     * which is a queue filling. This is the signal that moves before loss
     * does. It is a difference of differences, so the two clocks involved need
     * only agree on rate, never on offset.
     */
    std::int64_t delay_gradient_us = 0;
    std::uint32_t delivered_packets = 0;
    std::uint32_t lost_packets = 0;
  };

  /**
   * @brief Bounded record of what was sent, so feedback can be matched to it.
   *
   * Sized to cover well over a second of video at any sane packet rate. The
   * window wraps rather than growing: feedback that arrives after its packet
   * has been overwritten is too old to be worth acting on.
   */
  class packet_send_history_t {
  public:
    static constexpr std::size_t capacity = 4096;

    void record(
      std::uint16_t first_sequence_number,
      std::uint16_t packet_count,
      std::uint32_t wire_bytes_per_packet,
      feedback_time_point_t sent_at
    );

    struct record_t {
      bool known = false;
      std::uint32_t wire_bytes = 0;
      feedback_time_point_t sent_at {};
    };

    [[nodiscard]] record_t find(std::uint16_t sequence_number) const;

    void reset();

  private:
    struct slot_t {
      std::uint16_t sequence_number = 0;
      bool occupied = false;
      std::uint32_t wire_bytes = 0;
      feedback_time_point_t sent_at {};
    };

    std::array<slot_t, capacity> slots_ {};
  };

  /**
   * @brief Derive delivery rate and delay trend from one report.
   *
   * Packets the history no longer holds are skipped rather than guessed at:
   * a rate computed from packets whose size is unknown would be fiction.
   */
  [[nodiscard]] delivery_estimate_t analyze_packet_feedback(
    const packet_feedback_report_t &report,
    const packet_send_history_t &history
  );

}  // namespace stream::congestion
