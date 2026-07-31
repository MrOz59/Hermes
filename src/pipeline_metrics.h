/**
 * @file src/pipeline_metrics.h
 * @brief Bounded, deterministic aggregation for live video pipeline metrics.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace video {

  enum class pipeline_drop_reason_e : uint8_t {
    capture_replaced,
    encode,
    encoded_queue,
    send_deadline,
    packet_deadline,
    recovery_wait,
    reference_superseded,
  };

  struct pipeline_queue_metrics_t {
    struct queue_t {
      uint64_t active_instances = 0;
      uint64_t depth = 0;
      uint64_t capacity = 0;
      uint64_t high_watermark = 0;
      uint64_t overflow_events = 0;
      uint64_t dropped_elements = 0;
    };

    queue_t capture_contexts;
    queue_t encode_session_contexts;
  };

  struct pipeline_latency_metrics_t {
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
  };

  struct network_pipeline_metrics_t {
    bool valid = false;
    pipeline_latency_metrics_t send_queue;
    pipeline_latency_metrics_t packetization;
    pipeline_latency_metrics_t fec;
    pipeline_latency_metrics_t pacer;
    pipeline_latency_metrics_t send;
    pipeline_latency_metrics_t capture_to_last_send;
    /// Estimated upper bound, not a measurement: UDP/IP header sizes are
    /// synthesized and shards are counted before the send loop, so partial
    /// sends are still included. Use for diagnostics, not accounting.
    double wire_bitrate_kbps = 0.0;
    double fec_overhead_percent = 0.0;
    uint64_t window_sequence = 0;
    double window_duration_ms = 0.0;
    uint64_t window_frames = 0;
    uint64_t sampled_frames = 0;
    uint64_t data_shards = 0;
    uint64_t fec_shards = 0;
  };

  /**
   * @brief Last congestion-control state published for a session.
   *
   * Unlike the frame metrics this is not windowed here: the controller already
   * smooths its own estimate over a sample window, so re-averaging it would
   * only add lag.
   *
   * The two signals become available independently. Round trip comes from the
   * transport as soon as the control connection has timed one, while the loss
   * view needs enough client feedback to be conclusive -- so `valid` means the
   * block has something measured in it, and `loss_estimate_valid` is what says
   * the loss and capacity numbers can be believed rather than being the zeros
   * of a session nothing is known about yet.
   */
  struct congestion_pipeline_metrics_t {
    bool valid = false;
    /// True when an adapting controller is driving the session.
    bool adaptive = false;
    /// The loss ratios and the capacity estimate are conclusive.
    bool loss_estimate_valid = false;
    /// The path is queueing, so protection is held rather than raised.
    bool queue_congested = false;
    double loss_percent = 0.0;
    double unrecovered_loss_percent = 0.0;
    double clean_frame_percent = 0.0;
    uint64_t observed_frames = 0;
    uint64_t unrecovered_frames = 0;
    double fec_percent = 0.0;
    double key_frame_fec_percent = 0.0;
    double configured_fec_percent = 0.0;
    /// Advisory: what the path looks able to carry. Nothing applies it yet.
    double available_bitrate_kbps = 0.0;
    /// What the encoder was actually configured with.
    double configured_bitrate_kbps = 0.0;
    /// Transport round trip, and how much of it is queueing above the
    /// smallest round trip seen recently. Zero when nothing measured one.
    double rtt_ms = 0.0;
    double queue_delay_ms = 0.0;
  };

  /**
   * @brief Published snapshot of one completed metrics window.
   *
   * Latency fields use milliseconds at this diagnostics boundary. Pipeline
   * code should continue to use std::chrono durations internally.
   */
  struct pipeline_metrics_t {
    bool valid = false;
    double encode_ms = 0.0;
    double encode_p50_ms = 0.0;
    double encode_p95_ms = 0.0;
    double encode_p99_ms = 0.0;
    double capture_to_encode_ms = 0.0;
    double capture_to_encode_p50_ms = 0.0;
    double capture_to_encode_p95_ms = 0.0;
    double capture_to_encode_p99_ms = 0.0;
    double fps = 0.0;
    double bitrate_kbps = 0.0;
    uint64_t window_sequence = 0;
    double window_duration_ms = 0.0;
    uint64_t window_frames = 0;
    uint64_t sampled_frames = 0;
    uint64_t frames_encoded = 0;
    uint64_t frames_dropped = 0;
    uint64_t frames_replaced_before_encode = 0;
    uint64_t frames_dropped_encode = 0;
    uint64_t frames_dropped_encoded_queue = 0;
    uint64_t frames_dropped_send_deadline = 0;
    uint64_t frames_dropped_packet_deadline = 0;
    uint64_t frames_dropped_recovery_wait = 0;
    uint64_t frames_dropped_reference_superseded = 0;
    uint64_t idr_requests_accepted = 0;
    uint64_t idr_requests_rate_limited = 0;
    int width = 0;
    int height = 0;
    network_pipeline_metrics_t network;
    congestion_pipeline_metrics_t congestion;
  };

  /**
   * @brief Aggregates a bounded sample of per-frame timings.
   *
   * The collector performs no dynamic allocation while recording frames.
   * Callers provide the monotonic time so tests and replay tools can advance
   * the window deterministically. Thread safety is owned by the caller.
   */
  class pipeline_metrics_collector_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using duration_t = std::chrono::duration<double, std::milli>;

    static constexpr std::size_t max_window_samples = 512;
    static constexpr auto publish_interval = std::chrono::seconds {1};

    explicit pipeline_metrics_collector_t(clock_t::time_point now = clock_t::now());

    void reset(clock_t::time_point now = clock_t::now());
    void set_resolution(int width, int height);
    void record_frame(
      duration_t encode_time,
      duration_t capture_to_encode_time,
      std::size_t packet_bytes,
      clock_t::time_point now = clock_t::now()
    );
    void record_drop(pipeline_drop_reason_e reason = pipeline_drop_reason_e::encode, uint64_t count = 1);
    void record_idr_request(bool accepted, uint64_t count = 1);
    void record_network_frame(
      duration_t send_queue_time,
      duration_t packetization_time,
      duration_t fec_time,
      duration_t pacer_time,
      duration_t send_time,
      duration_t capture_to_last_send_time,
      std::size_t wire_bytes,
      uint64_t data_shards,
      uint64_t fec_shards,
      clock_t::time_point now = clock_t::now()
    );
    void record_congestion(const congestion_pipeline_metrics_t &congestion);

    [[nodiscard]] pipeline_metrics_t snapshot() const;

  private:
    struct frame_sample_t {
      double encode_ms = 0.0;
      double capture_to_encode_ms = 0.0;
    };

    struct network_sample_t {
      double send_queue_ms = 0.0;
      double packetization_ms = 0.0;
      double fec_ms = 0.0;
      double pacer_ms = 0.0;
      double send_ms = 0.0;
      double capture_to_last_send_ms = 0.0;
    };

    void publish(clock_t::time_point now);
    void publish_network(clock_t::time_point now);

    std::array<frame_sample_t, max_window_samples> samples_ {};
    std::array<network_sample_t, max_window_samples> network_samples_ {};
    std::size_t sample_count_ = 0;
    std::size_t network_sample_count_ = 0;
    uint64_t window_frames_ = 0;
    uint64_t window_bytes_ = 0;
    uint64_t network_window_frames_ = 0;
    uint64_t network_window_bytes_ = 0;
    uint64_t network_window_data_shards_ = 0;
    uint64_t network_window_fec_shards_ = 0;
    uint64_t frames_encoded_ = 0;
    uint64_t frames_dropped_ = 0;
    uint64_t frames_replaced_before_encode_ = 0;
    uint64_t frames_dropped_encode_ = 0;
    uint64_t frames_dropped_encoded_queue_ = 0;
    uint64_t frames_dropped_send_deadline_ = 0;
    uint64_t frames_dropped_packet_deadline_ = 0;
    uint64_t frames_dropped_recovery_wait_ = 0;
    uint64_t frames_dropped_reference_superseded_ = 0;
    uint64_t idr_requests_accepted_ = 0;
    uint64_t idr_requests_rate_limited_ = 0;
    uint64_t published_windows_ = 0;
    uint64_t network_published_windows_ = 0;
    int width_ = 0;
    int height_ = 0;
    clock_t::time_point window_start_;
    clock_t::time_point network_window_start_;
    pipeline_metrics_t published_;
    network_pipeline_metrics_t network_published_;
    congestion_pipeline_metrics_t congestion_published_;
  };

  /**
   * @brief Bounded registry of independent per-session metric collectors.
   *
   * Session IDs are opaque process-local values. Registration and removal are
   * lifecycle operations; frame recording never allocates. Thread safety is
   * owned by the caller so production can share one lock with snapshots.
   */
  class pipeline_metrics_registry_t {
  public:
    using session_id_t = std::uintptr_t;
    using clock_t = pipeline_metrics_collector_t::clock_t;
    using duration_t = pipeline_metrics_collector_t::duration_t;

    static constexpr std::size_t max_sessions = 8;

    explicit pipeline_metrics_registry_t(clock_t::time_point now = clock_t::now());

    bool register_session(session_id_t session_id, clock_t::time_point now = clock_t::now());
    bool unregister_session(session_id_t session_id);
    void reset(clock_t::time_point now = clock_t::now());

    bool set_resolution(session_id_t session_id, int width, int height);
    bool record_frame(
      session_id_t session_id,
      duration_t encode_time,
      duration_t capture_to_encode_time,
      std::size_t packet_bytes,
      clock_t::time_point now = clock_t::now()
    );
    bool record_drop(
      session_id_t session_id,
      pipeline_drop_reason_e reason = pipeline_drop_reason_e::encode,
      uint64_t count = 1
    );
    bool record_idr_request(
      session_id_t session_id,
      bool accepted,
      uint64_t count = 1
    );
    bool record_network_frame(
      session_id_t session_id,
      duration_t send_queue_time,
      duration_t packetization_time,
      duration_t fec_time,
      duration_t pacer_time,
      duration_t send_time,
      duration_t capture_to_last_send_time,
      std::size_t wire_bytes,
      uint64_t data_shards,
      uint64_t fec_shards,
      clock_t::time_point now = clock_t::now()
    );
    bool record_congestion(
      session_id_t session_id,
      const congestion_pipeline_metrics_t &congestion
    );

    [[nodiscard]] std::optional<pipeline_metrics_t> snapshot(session_id_t session_id) const;
    [[nodiscard]] pipeline_metrics_t aggregate_snapshot() const;
    [[nodiscard]] std::size_t active_sessions() const;

  private:
    struct entry_t {
      bool active = false;
      session_id_t session_id = 0;
      pipeline_metrics_collector_t collector;
    };

    entry_t *find(session_id_t session_id);
    const entry_t *find(session_id_t session_id) const;

    std::array<entry_t, max_sessions> entries_;
    pipeline_metrics_collector_t aggregate_;
    std::size_t active_sessions_ = 0;
    bool aggregate_resolution_set_ = false;
    bool aggregate_resolution_mixed_ = false;
    int aggregate_width_ = 0;
    int aggregate_height_ = 0;
  };

}  // namespace video
