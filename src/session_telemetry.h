/**
 * @file src/session_telemetry.h
 * @brief Injectable, typed boundary for host-side session telemetry.
 */
#pragma once

#include "pipeline_metrics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace video {

  using session_telemetry_id_t = pipeline_metrics_registry_t::session_id_t;
  using session_telemetry_clock_t = pipeline_metrics_registry_t::clock_t;
  using session_telemetry_duration_t = pipeline_metrics_registry_t::duration_t;

  struct session_resolution_event_t {
    session_telemetry_id_t session_id = 0;
    int width = 0;
    int height = 0;
  };

  struct encoded_frame_telemetry_t {
    session_telemetry_id_t session_id = 0;
    session_telemetry_duration_t encode_time {};
    session_telemetry_duration_t capture_to_encode_time {};
    std::size_t packet_bytes = 0;
    std::optional<session_telemetry_clock_t::time_point> recorded_at;
  };

  struct network_frame_telemetry_t {
    session_telemetry_id_t session_id = 0;
    session_telemetry_duration_t send_queue_time {};
    session_telemetry_duration_t packetization_time {};
    session_telemetry_duration_t fec_time {};
    session_telemetry_duration_t pacer_time {};
    session_telemetry_duration_t send_time {};
    session_telemetry_duration_t capture_to_last_send_time {};
    std::size_t wire_bytes = 0;
    uint64_t data_shards = 0;
    uint64_t fec_shards = 0;
    std::optional<session_telemetry_clock_t::time_point> recorded_at;
  };

  struct frame_drop_telemetry_t {
    session_telemetry_id_t session_id = 0;
    pipeline_drop_reason_e reason = pipeline_drop_reason_e::encode;
    uint64_t count = 1;
  };

  struct idr_request_telemetry_t {
    session_telemetry_id_t session_id = 0;
    bool accepted = false;
    uint64_t count = 1;
  };

  struct congestion_telemetry_t {
    session_telemetry_id_t session_id = 0;
    congestion_pipeline_metrics_t state;
  };

  /**
   * @brief Local telemetry boundary shared by session pipeline producers.
   *
   * Calls may originate from RTSP lifecycle, encode, broadcaster, and
   * diagnostics threads. Implementations must therefore be thread-safe and
   * must not call producers back while holding an internal lock. Events are
   * process-local observations; implementing this interface does not make
   * them transport feedback or change the GameStream wire format.
   */
  class ISessionTelemetry {
  public:
    virtual ~ISessionTelemetry() = default;

    virtual bool start_session(
      session_telemetry_id_t session_id,
      std::optional<session_telemetry_clock_t::time_point> now =
        std::nullopt
    ) = 0;
    virtual bool end_session(session_telemetry_id_t session_id) = 0;

    virtual bool publish_resolution(
      const session_resolution_event_t &event
    ) = 0;
    virtual bool publish_encoded_frame(
      const encoded_frame_telemetry_t &event
    ) = 0;
    virtual bool publish_network_frame(
      const network_frame_telemetry_t &event
    ) = 0;
    virtual bool publish_frame_drop(
      const frame_drop_telemetry_t &event
    ) = 0;
    virtual bool publish_idr_request(
      const idr_request_telemetry_t &event
    ) = 0;
    /**
     * @brief Publish the controller's current view of the client's path.
     *
     * Called from the control thread on feedback rather than per frame: the
     * value only changes when feedback arrives.
     */
    virtual bool publish_congestion(
      const congestion_telemetry_t &event
    ) = 0;

    [[nodiscard]] virtual std::optional<pipeline_metrics_t> snapshot(
      session_telemetry_id_t session_id
    ) const = 0;
    [[nodiscard]] virtual pipeline_metrics_t aggregate_snapshot() const = 0;
  };

  /**
   * @brief Thread-safe H0 collector exposed through the H1 telemetry boundary.
   *
   * Recording retains the existing fixed session and sample capacities and
   * performs no dynamic allocation in the frame hot path.
   */
  class bounded_session_telemetry_t final: public ISessionTelemetry {
  public:
    explicit bounded_session_telemetry_t(
      session_telemetry_clock_t::time_point now =
        session_telemetry_clock_t::now()
    );

    bool start_session(
      session_telemetry_id_t session_id,
      std::optional<session_telemetry_clock_t::time_point> now =
        std::nullopt
    ) override;
    bool end_session(session_telemetry_id_t session_id) override;
    bool publish_resolution(
      const session_resolution_event_t &event
    ) override;
    bool publish_encoded_frame(
      const encoded_frame_telemetry_t &event
    ) override;
    bool publish_network_frame(
      const network_frame_telemetry_t &event
    ) override;
    bool publish_frame_drop(
      const frame_drop_telemetry_t &event
    ) override;
    bool publish_idr_request(
      const idr_request_telemetry_t &event
    ) override;
    bool publish_congestion(
      const congestion_telemetry_t &event
    ) override;
    [[nodiscard]] std::optional<pipeline_metrics_t> snapshot(
      session_telemetry_id_t session_id
    ) const override;
    [[nodiscard]] pipeline_metrics_t aggregate_snapshot() const override;

  private:
    mutable std::mutex mutex_;
    pipeline_metrics_registry_t metrics_;
  };

  /**
   * @brief Adapter from the current opaque GameStream session API to H1.
   *
   * Keeping this conversion at the legacy boundary lets existing capture,
   * encode, RTSP, and broadcaster call sites remain unchanged while the sink
   * can be replaced by a fake or a future telemetry backend.
   */
  class legacy_session_telemetry_adapter_t {
  public:
    explicit legacy_session_telemetry_adapter_t(
      ISessionTelemetry &telemetry
    ) noexcept;

    bool register_session(
      void *session,
      std::optional<session_telemetry_clock_t::time_point> now =
        std::nullopt
    );
    void unregister_session(void *session);
    void set_resolution(void *session, int width, int height);
    void record_encoded_frame(
      void *session,
      double encode_ms,
      double capture_to_encode_ms,
      std::size_t packet_bytes
    );
    void record_network_frame(
      void *session,
      double send_queue_ms,
      double packetization_ms,
      double fec_ms,
      double pacer_ms,
      double send_ms,
      double capture_to_last_send_ms,
      std::size_t wire_bytes,
      uint64_t data_shards,
      uint64_t fec_shards
    );
    void record_frame_drop(
      void *session,
      pipeline_drop_reason_e reason,
      uint64_t count
    );
    void record_idr_request(
      void *session,
      bool accepted,
      uint64_t count
    );
    void record_congestion(
      void *session,
      const congestion_pipeline_metrics_t &state
    );
    [[nodiscard]] std::optional<pipeline_metrics_t> snapshot(
      void *session
    ) const;
    [[nodiscard]] pipeline_metrics_t aggregate_snapshot() const;

  private:
    static session_telemetry_id_t session_id(void *session) noexcept;

    ISessionTelemetry &telemetry_;
  };

}  // namespace video
