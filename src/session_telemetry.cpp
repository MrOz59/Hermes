/**
 * @file src/session_telemetry.cpp
 * @brief Host-side session telemetry implementations and legacy adapter.
 */

#include "session_telemetry.h"

namespace video {

  bounded_session_telemetry_t::bounded_session_telemetry_t(
    session_telemetry_clock_t::time_point now
  ):
      metrics_ {now} {
  }

  bool bounded_session_telemetry_t::start_session(
    session_telemetry_id_t session_id,
    std::optional<session_telemetry_clock_t::time_point> now
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.register_session(
      session_id,
      now ? *now : session_telemetry_clock_t::now()
    );
  }

  bool bounded_session_telemetry_t::end_session(
    session_telemetry_id_t session_id
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.unregister_session(session_id);
  }

  bool bounded_session_telemetry_t::publish_resolution(
    const session_resolution_event_t &event
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.set_resolution(
      event.session_id,
      event.width,
      event.height
    );
  }

  bool bounded_session_telemetry_t::publish_encoded_frame(
    const encoded_frame_telemetry_t &event
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.record_frame(
      event.session_id,
      event.encode_time,
      event.capture_to_encode_time,
      event.packet_bytes,
      event.recorded_at ?
        *event.recorded_at :
        session_telemetry_clock_t::now()
    );
  }

  bool bounded_session_telemetry_t::publish_network_frame(
    const network_frame_telemetry_t &event
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.record_network_frame(
      event.session_id,
      event.send_queue_time,
      event.packetization_time,
      event.fec_time,
      event.pacer_time,
      event.send_time,
      event.capture_to_last_send_time,
      event.wire_bytes,
      event.data_shards,
      event.fec_shards,
      event.recorded_at ?
        *event.recorded_at :
        session_telemetry_clock_t::now()
    );
  }

  bool bounded_session_telemetry_t::publish_frame_drop(
    const frame_drop_telemetry_t &event
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.record_drop(
      event.session_id,
      event.reason,
      event.count
    );
  }

  bool bounded_session_telemetry_t::publish_idr_request(
    const idr_request_telemetry_t &event
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.record_idr_request(
      event.session_id,
      event.accepted,
      event.count
    );
  }

  bool bounded_session_telemetry_t::publish_congestion(
    const congestion_telemetry_t &event
  ) {
    std::lock_guard lock {mutex_};
    return metrics_.record_congestion(event.session_id, event.state);
  }

  std::optional<pipeline_metrics_t>
    bounded_session_telemetry_t::snapshot(
      session_telemetry_id_t session_id
    ) const {
    std::lock_guard lock {mutex_};
    return metrics_.snapshot(session_id);
  }

  pipeline_metrics_t
    bounded_session_telemetry_t::aggregate_snapshot() const {
    std::lock_guard lock {mutex_};
    return metrics_.aggregate_snapshot();
  }

  legacy_session_telemetry_adapter_t::
    legacy_session_telemetry_adapter_t(
      ISessionTelemetry &telemetry
    ) noexcept:
      telemetry_ {telemetry} {
  }

  bool legacy_session_telemetry_adapter_t::register_session(
    void *session,
    std::optional<session_telemetry_clock_t::time_point> now
  ) {
    return telemetry_.start_session(session_id(session), now);
  }

  void legacy_session_telemetry_adapter_t::unregister_session(
    void *session
  ) {
    telemetry_.end_session(session_id(session));
  }

  void legacy_session_telemetry_adapter_t::set_resolution(
    void *session,
    int width,
    int height
  ) {
    telemetry_.publish_resolution({
      .session_id = session_id(session),
      .width = width,
      .height = height,
    });
  }

  void legacy_session_telemetry_adapter_t::record_encoded_frame(
    void *session,
    double encode_ms,
    double capture_to_encode_ms,
    std::size_t packet_bytes
  ) {
    telemetry_.publish_encoded_frame({
      .session_id = session_id(session),
      .encode_time = session_telemetry_duration_t {encode_ms},
      .capture_to_encode_time =
        session_telemetry_duration_t {capture_to_encode_ms},
      .packet_bytes = packet_bytes,
    });
  }

  void legacy_session_telemetry_adapter_t::record_network_frame(
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
  ) {
    telemetry_.publish_network_frame({
      .session_id = session_id(session),
      .send_queue_time = session_telemetry_duration_t {send_queue_ms},
      .packetization_time =
        session_telemetry_duration_t {packetization_ms},
      .fec_time = session_telemetry_duration_t {fec_ms},
      .pacer_time = session_telemetry_duration_t {pacer_ms},
      .send_time = session_telemetry_duration_t {send_ms},
      .capture_to_last_send_time =
        session_telemetry_duration_t {capture_to_last_send_ms},
      .wire_bytes = wire_bytes,
      .data_shards = data_shards,
      .fec_shards = fec_shards,
    });
  }

  void legacy_session_telemetry_adapter_t::record_frame_drop(
    void *session,
    pipeline_drop_reason_e reason,
    uint64_t count
  ) {
    telemetry_.publish_frame_drop({
      .session_id = session_id(session),
      .reason = reason,
      .count = count,
    });
  }

  void legacy_session_telemetry_adapter_t::record_idr_request(
    void *session,
    bool accepted,
    uint64_t count
  ) {
    telemetry_.publish_idr_request({
      .session_id = session_id(session),
      .accepted = accepted,
      .count = count,
    });
  }

  void legacy_session_telemetry_adapter_t::record_congestion(
    void *session,
    const congestion_pipeline_metrics_t &state
  ) {
    telemetry_.publish_congestion({
      .session_id = session_id(session),
      .state = state,
    });
  }

  std::optional<pipeline_metrics_t>
    legacy_session_telemetry_adapter_t::snapshot(void *session) const {
    return telemetry_.snapshot(session_id(session));
  }

  pipeline_metrics_t
    legacy_session_telemetry_adapter_t::aggregate_snapshot() const {
    return telemetry_.aggregate_snapshot();
  }

  session_telemetry_id_t
    legacy_session_telemetry_adapter_t::session_id(
      void *session
    ) noexcept {
    return reinterpret_cast<session_telemetry_id_t>(session);
  }

}  // namespace video
