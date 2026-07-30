/**
 * @file tests/unit/test_session_telemetry.cpp
 * @brief Tests for the injectable Hermes session telemetry boundary.
 */

#include "../tests_common.h"

#include <src/session_telemetry.h>

namespace {

  class fake_session_telemetry_t final: public video::ISessionTelemetry {
  public:
    bool start_session(
      video::session_telemetry_id_t session_id,
      std::optional<video::session_telemetry_clock_t::time_point> now
    ) override {
      ++start_calls;
      last_session_id = session_id;
      last_started_at = now;
      return start_result;
    }

    bool end_session(video::session_telemetry_id_t session_id) override {
      ++end_calls;
      last_session_id = session_id;
      return true;
    }

    bool publish_resolution(
      const video::session_resolution_event_t &event
    ) override {
      ++resolution_calls;
      last_resolution = event;
      return true;
    }

    bool publish_encoded_frame(
      const video::encoded_frame_telemetry_t &event
    ) override {
      ++encoded_frame_calls;
      last_encoded_frame = event;
      return true;
    }

    bool publish_network_frame(
      const video::network_frame_telemetry_t &event
    ) override {
      ++network_frame_calls;
      last_network_frame = event;
      return true;
    }

    bool publish_frame_drop(
      const video::frame_drop_telemetry_t &event
    ) override {
      ++frame_drop_calls;
      last_frame_drop = event;
      return true;
    }

    bool publish_idr_request(
      const video::idr_request_telemetry_t &event
    ) override {
      ++idr_request_calls;
      last_idr_request = event;
      return true;
    }

    bool publish_congestion(
      const video::congestion_telemetry_t &event
    ) override {
      ++congestion_calls;
      last_congestion = event;
      return true;
    }

    std::optional<video::pipeline_metrics_t> snapshot(
      video::session_telemetry_id_t session_id
    ) const override {
      ++snapshot_calls;
      last_snapshot_session_id = session_id;
      return snapshot_result;
    }

    video::pipeline_metrics_t aggregate_snapshot() const override {
      ++aggregate_snapshot_calls;
      return aggregate_result;
    }

    bool start_result = true;
    video::pipeline_metrics_t snapshot_result {
      .valid = true,
      .width = 1920,
      .height = 1080,
    };
    video::pipeline_metrics_t aggregate_result {
      .valid = true,
      .width = 0,
      .height = 0,
    };
    video::session_telemetry_id_t last_session_id = 0;
    mutable video::session_telemetry_id_t last_snapshot_session_id = 0;
    std::optional<video::session_telemetry_clock_t::time_point>
      last_started_at;
    video::session_resolution_event_t last_resolution;
    video::encoded_frame_telemetry_t last_encoded_frame;
    video::network_frame_telemetry_t last_network_frame;
    video::frame_drop_telemetry_t last_frame_drop;
    video::idr_request_telemetry_t last_idr_request;
    video::congestion_telemetry_t last_congestion;
    int start_calls = 0;
    int end_calls = 0;
    int resolution_calls = 0;
    int encoded_frame_calls = 0;
    int network_frame_calls = 0;
    int frame_drop_calls = 0;
    int idr_request_calls = 0;
    int congestion_calls = 0;
    mutable int snapshot_calls = 0;
    mutable int aggregate_snapshot_calls = 0;
  };

}  // namespace

TEST(SessionTelemetryTest, LegacyAdapterPublishesTypedEventsToFakeSink) {
  fake_session_telemetry_t fake;
  video::ISessionTelemetry &sink = fake;
  video::legacy_session_telemetry_adapter_t adapter {sink};
  const video::session_telemetry_clock_t::time_point started_at {};
  int opaque_session;
  void *session = &opaque_session;
  const auto expected_id =
    reinterpret_cast<video::session_telemetry_id_t>(session);

  ASSERT_TRUE(adapter.register_session(session, started_at));
  adapter.set_resolution(session, 2560, 1440);
  adapter.record_encoded_frame(session, 1.25, 2.5, 4096);
  adapter.record_network_frame(
    session,
    3.0,
    4.0,
    5.0,
    6.0,
    7.0,
    8.0,
    8192,
    10,
    2
  );
  adapter.record_frame_drop(
    session,
    video::pipeline_drop_reason_e::encoded_queue,
    3
  );
  adapter.record_idr_request(session, false, 4);

  const auto snapshot = adapter.snapshot(session);
  const auto aggregate = adapter.aggregate_snapshot();
  adapter.unregister_session(session);

  EXPECT_EQ(fake.start_calls, 1);
  EXPECT_EQ(fake.end_calls, 1);
  EXPECT_EQ(fake.last_session_id, expected_id);
  ASSERT_TRUE(fake.last_started_at.has_value());
  EXPECT_EQ(*fake.last_started_at, started_at);

  ASSERT_EQ(fake.resolution_calls, 1);
  EXPECT_EQ(fake.last_resolution.session_id, expected_id);
  EXPECT_EQ(fake.last_resolution.width, 2560);
  EXPECT_EQ(fake.last_resolution.height, 1440);

  ASSERT_EQ(fake.encoded_frame_calls, 1);
  EXPECT_EQ(fake.last_encoded_frame.session_id, expected_id);
  EXPECT_DOUBLE_EQ(fake.last_encoded_frame.encode_time.count(), 1.25);
  EXPECT_DOUBLE_EQ(
    fake.last_encoded_frame.capture_to_encode_time.count(),
    2.5
  );
  EXPECT_EQ(fake.last_encoded_frame.packet_bytes, 4096);

  ASSERT_EQ(fake.network_frame_calls, 1);
  EXPECT_EQ(fake.last_network_frame.session_id, expected_id);
  EXPECT_DOUBLE_EQ(fake.last_network_frame.send_queue_time.count(), 3.0);
  EXPECT_DOUBLE_EQ(fake.last_network_frame.packetization_time.count(), 4.0);
  EXPECT_DOUBLE_EQ(fake.last_network_frame.fec_time.count(), 5.0);
  EXPECT_DOUBLE_EQ(fake.last_network_frame.pacer_time.count(), 6.0);
  EXPECT_DOUBLE_EQ(fake.last_network_frame.send_time.count(), 7.0);
  EXPECT_DOUBLE_EQ(
    fake.last_network_frame.capture_to_last_send_time.count(),
    8.0
  );
  EXPECT_EQ(fake.last_network_frame.wire_bytes, 8192);
  EXPECT_EQ(fake.last_network_frame.data_shards, 10);
  EXPECT_EQ(fake.last_network_frame.fec_shards, 2);

  ASSERT_EQ(fake.frame_drop_calls, 1);
  EXPECT_EQ(fake.last_frame_drop.session_id, expected_id);
  EXPECT_EQ(
    fake.last_frame_drop.reason,
    video::pipeline_drop_reason_e::encoded_queue
  );
  EXPECT_EQ(fake.last_frame_drop.count, 3);

  ASSERT_EQ(fake.idr_request_calls, 1);
  EXPECT_EQ(fake.last_idr_request.session_id, expected_id);
  EXPECT_FALSE(fake.last_idr_request.accepted);
  EXPECT_EQ(fake.last_idr_request.count, 4);

  ASSERT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->valid);
  EXPECT_EQ(snapshot->width, 1920);
  EXPECT_EQ(fake.snapshot_calls, 1);
  EXPECT_EQ(fake.last_snapshot_session_id, expected_id);
  EXPECT_TRUE(aggregate.valid);
  EXPECT_EQ(fake.aggregate_snapshot_calls, 1);
}

TEST(SessionTelemetryTest, BoundedSinkPreservesPerSessionMetrics) {
  const video::session_telemetry_clock_t::time_point start {};
  video::bounded_session_telemetry_t bounded {start};
  video::ISessionTelemetry &sink = bounded;
  constexpr video::session_telemetry_id_t session_id = 42;

  ASSERT_TRUE(sink.start_session(session_id, start));
  ASSERT_TRUE(sink.publish_resolution({
    .session_id = session_id,
    .width = 1920,
    .height = 1080,
  }));
  ASSERT_TRUE(sink.publish_encoded_frame({
    .session_id = session_id,
    .encode_time = video::session_telemetry_duration_t {1.0},
    .capture_to_encode_time =
      video::session_telemetry_duration_t {2.0},
    .packet_bytes = 1000,
    .recorded_at =
      start + video::pipeline_metrics_collector_t::publish_interval,
  }));
  ASSERT_TRUE(sink.publish_network_frame({
    .session_id = session_id,
    .send_queue_time = video::session_telemetry_duration_t {3.0},
    .packetization_time = video::session_telemetry_duration_t {4.0},
    .fec_time = video::session_telemetry_duration_t {5.0},
    .pacer_time = video::session_telemetry_duration_t {6.0},
    .send_time = video::session_telemetry_duration_t {7.0},
    .capture_to_last_send_time =
      video::session_telemetry_duration_t {8.0},
    .wire_bytes = 1200,
    .data_shards = 10,
    .fec_shards = 2,
    .recorded_at =
      start + video::pipeline_metrics_collector_t::publish_interval,
  }));
  ASSERT_TRUE(sink.publish_idr_request({
    .session_id = session_id,
    .accepted = true,
    .count = 2,
  }));
  ASSERT_TRUE(sink.publish_idr_request({
    .session_id = session_id,
    .accepted = false,
    .count = 3,
  }));

  const auto metrics = sink.snapshot(session_id);
  ASSERT_TRUE(metrics.has_value());
  ASSERT_TRUE(metrics->valid);
  ASSERT_TRUE(metrics->network.valid);
  EXPECT_EQ(metrics->width, 1920);
  EXPECT_EQ(metrics->height, 1080);
  EXPECT_DOUBLE_EQ(metrics->encode_ms, 1.0);
  EXPECT_DOUBLE_EQ(metrics->network.send_queue.mean_ms, 3.0);
  EXPECT_DOUBLE_EQ(metrics->network.fec_overhead_percent, 20.0);
  EXPECT_EQ(metrics->idr_requests_accepted, 2);
  EXPECT_EQ(metrics->idr_requests_rate_limited, 3);

  ASSERT_TRUE(sink.end_session(session_id));
  EXPECT_FALSE(sink.snapshot(session_id).has_value());
}

// Congestion state is published on feedback rather than per frame, so it must
// survive in the snapshot without waiting for a frame window to close.
TEST(SessionTelemetryTest, BoundedSinkPublishesCongestionImmediately) {
  const video::session_telemetry_clock_t::time_point start {};
  video::bounded_session_telemetry_t bounded {start};
  video::ISessionTelemetry &sink = bounded;
  constexpr video::session_telemetry_id_t session_id = 7;

  ASSERT_TRUE(sink.start_session(session_id, start));
  ASSERT_TRUE(sink.publish_congestion({
    .session_id = session_id,
    .state = {
      .valid = true,
      .adaptive = true,
      .loss_percent = 4.0,
      .unrecovered_loss_percent = 1.5,
      .clean_frame_percent = 92.0,
      .observed_frames = 120,
      .unrecovered_frames = 2,
      .fec_percent = 20.0,
      .key_frame_fec_percent = 30.0,
      .configured_fec_percent = 10.0,
      .available_bitrate_kbps = 17'000.0,
      .configured_bitrate_kbps = 20'000.0,
    },
  }));

  const auto snapshot = sink.snapshot(session_id);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->congestion.valid);
  EXPECT_TRUE(snapshot->congestion.adaptive);
  EXPECT_DOUBLE_EQ(snapshot->congestion.unrecovered_loss_percent, 1.5);
  EXPECT_EQ(snapshot->congestion.observed_frames, 120u);
  EXPECT_DOUBLE_EQ(snapshot->congestion.key_frame_fec_percent, 30.0);
  EXPECT_DOUBLE_EQ(snapshot->congestion.available_bitrate_kbps, 17'000.0);
  EXPECT_DOUBLE_EQ(snapshot->congestion.configured_bitrate_kbps, 20'000.0);

  // An unknown session must not silently succeed: diagnostics would then show
  // one client's path under another's.
  EXPECT_FALSE(sink.publish_congestion({
    .session_id = session_id + 1,
    .state = {.valid = true},
  }));
}

TEST(SessionTelemetryTest, LegacyAdapterForwardsCongestionState) {
  fake_session_telemetry_t fake;
  video::ISessionTelemetry &sink = fake;
  video::legacy_session_telemetry_adapter_t adapter {sink};
  int session_marker = 0;

  adapter.record_congestion(
    &session_marker,
    {
      .valid = true,
      .unrecovered_loss_percent = 3.25,
      .available_bitrate_kbps = 12'000.0,
    }
  );

  ASSERT_EQ(fake.congestion_calls, 1);
  EXPECT_EQ(
    fake.last_congestion.session_id,
    reinterpret_cast<video::session_telemetry_id_t>(&session_marker)
  );
  EXPECT_TRUE(fake.last_congestion.state.valid);
  EXPECT_DOUBLE_EQ(fake.last_congestion.state.unrecovered_loss_percent, 3.25);
  EXPECT_DOUBLE_EQ(fake.last_congestion.state.available_bitrate_kbps, 12'000.0);
}
