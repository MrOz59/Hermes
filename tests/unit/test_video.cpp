/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <src/frame_queue_policy.h>
#include <src/pipeline_metrics.h>
#include <src/video.h>

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#ifdef __linux__
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

// Verify that probing records a coherent, diagnostics-ready encoder status,
// including the per-encoder attempt list used to explain a software fallback.
struct EncoderStatusTest: PlatformTestSuite {};

TEST_F(EncoderStatusTest, ProbeRecordsAttempts) {
  if (video::probe_encoders() != 0) {
    GTEST_SKIP() << "No encoder/display available to probe";
  }

  const auto status = video::get_encoder_status();
  ASSERT_TRUE(status.probed);
  ASSERT_FALSE(status.encoder.empty());

  // The selected encoder must be the single attempt flagged `selected`, must
  // appear last, and its name must match the chosen encoder.
  ASSERT_FALSE(status.attempts.empty());
  int selected_count = 0;
  for (const auto &attempt : status.attempts) {
    ASSERT_FALSE(attempt.name.empty());
    ASSERT_FALSE(attempt.outcome.empty());
    if (attempt.selected) {
      ++selected_count;
    }
  }
  ASSERT_EQ(selected_count, 1);
  ASSERT_TRUE(status.attempts.back().selected);
  ASSERT_EQ(status.attempts.back().name, status.encoder);
  ASSERT_EQ(status.attempts.back().outcome, "selected");

  // hardware <-> "software" name, and the fallback flag is only set when a
  // hardware encoder was actually rejected before landing on software.
  ASSERT_EQ(status.hardware, status.encoder != "software");
  if (status.fell_back_to_software) {
    ASSERT_FALSE(status.hardware);
    ASSERT_GT(status.attempts.size(), 1u);
  }
}

TEST(PipelineMetricsCollectorTest, PublishesDeterministicLatencyPercentiles) {
  using collector_t = video::pipeline_metrics_collector_t;
  const collector_t::clock_t::time_point start {};
  collector_t collector {start};
  collector.set_resolution(1920, 1080);

  for (int sample = 1; sample <= 100; ++sample) {
    const auto now =
      sample == 100 ? start + collector_t::publish_interval :
                      start + std::chrono::milliseconds {sample * 9};
    collector.record_frame(
      collector_t::duration_t {static_cast<double>(sample)},
      collector_t::duration_t {static_cast<double>(sample * 2)},
      1000,
      now
    );
  }

  const auto metrics = collector.snapshot();
  ASSERT_TRUE(metrics.valid);
  EXPECT_DOUBLE_EQ(metrics.encode_ms, 50.5);
  EXPECT_DOUBLE_EQ(metrics.encode_p50_ms, 50.0);
  EXPECT_DOUBLE_EQ(metrics.encode_p95_ms, 95.0);
  EXPECT_DOUBLE_EQ(metrics.encode_p99_ms, 99.0);
  EXPECT_DOUBLE_EQ(metrics.capture_to_encode_ms, 101.0);
  EXPECT_DOUBLE_EQ(metrics.capture_to_encode_p50_ms, 100.0);
  EXPECT_DOUBLE_EQ(metrics.capture_to_encode_p95_ms, 190.0);
  EXPECT_DOUBLE_EQ(metrics.capture_to_encode_p99_ms, 198.0);
  EXPECT_DOUBLE_EQ(metrics.fps, 100.0);
  EXPECT_DOUBLE_EQ(metrics.bitrate_kbps, 800.0);
  EXPECT_EQ(metrics.window_sequence, 1);
  EXPECT_DOUBLE_EQ(metrics.window_duration_ms, 1000.0);
  EXPECT_EQ(metrics.window_frames, 100);
  EXPECT_EQ(metrics.sampled_frames, 100);
  EXPECT_EQ(metrics.frames_encoded, 100);
  EXPECT_EQ(metrics.frames_dropped, 0);
  EXPECT_EQ(metrics.width, 1920);
  EXPECT_EQ(metrics.height, 1080);

  collector.record_frame(
    collector_t::duration_t {1.0},
    collector_t::duration_t {2.0},
    1000,
    start + std::chrono::seconds {2}
  );
  EXPECT_EQ(collector.snapshot().window_sequence, 2);
}

TEST(PipelineMetricsCollectorTest, RemainsInvalidUntilAWindowCompletes) {
  using collector_t = video::pipeline_metrics_collector_t;
  const collector_t::clock_t::time_point start {};
  collector_t collector {start};

  collector.record_frame(
    collector_t::duration_t {1.0},
    collector_t::duration_t {2.0},
    1000,
    start + std::chrono::milliseconds {999}
  );

  EXPECT_FALSE(collector.snapshot().valid);
}

TEST(PipelineMetricsCollectorTest, AttributesDropsToTheirPipelineStage) {
  using collector_t = video::pipeline_metrics_collector_t;
  collector_t collector;

  collector.record_drop(video::pipeline_drop_reason_e::capture_replaced, 2);
  collector.record_drop(video::pipeline_drop_reason_e::encode);
  collector.record_drop(video::pipeline_drop_reason_e::encoded_queue, 3);
  collector.record_drop(video::pipeline_drop_reason_e::send_deadline, 4);
  collector.record_drop(video::pipeline_drop_reason_e::packet_deadline, 7);
  collector.record_drop(video::pipeline_drop_reason_e::recovery_wait, 5);
  collector.record_drop(
    video::pipeline_drop_reason_e::reference_superseded,
    6
  );

  const auto metrics = collector.snapshot();
  EXPECT_EQ(metrics.frames_dropped, 28);
  EXPECT_EQ(metrics.frames_replaced_before_encode, 2);
  EXPECT_EQ(metrics.frames_dropped_encode, 1);
  EXPECT_EQ(metrics.frames_dropped_encoded_queue, 3);
  EXPECT_EQ(metrics.frames_dropped_send_deadline, 4);
  EXPECT_EQ(metrics.frames_dropped_packet_deadline, 7);
  EXPECT_EQ(metrics.frames_dropped_recovery_wait, 5);
  EXPECT_EQ(metrics.frames_dropped_reference_superseded, 6);
}

TEST(PipelineMetricsCollectorTest, CountsAcceptedAndRateLimitedIdrRequests) {
  video::pipeline_metrics_collector_t collector;

  collector.record_idr_request(true, 2);
  collector.record_idr_request(false, 3);

  const auto metrics = collector.snapshot();
  EXPECT_EQ(metrics.idr_requests_accepted, 2);
  EXPECT_EQ(metrics.idr_requests_rate_limited, 3);
}

TEST(PipelineMetricsCollectorTest, PublishesDeterministicNetworkPercentiles) {
  using collector_t = video::pipeline_metrics_collector_t;
  const collector_t::clock_t::time_point start {};
  collector_t collector {start};

  for (int sample = 1; sample <= 100; ++sample) {
    const auto now =
      sample == 100 ? start + collector_t::publish_interval :
                      start + std::chrono::milliseconds {sample * 9};
    collector.record_network_frame(
      collector_t::duration_t {static_cast<double>(sample)},
      collector_t::duration_t {static_cast<double>(sample * 2)},
      collector_t::duration_t {static_cast<double>(sample * 3)},
      collector_t::duration_t {static_cast<double>(sample * 4)},
      collector_t::duration_t {static_cast<double>(sample * 5)},
      collector_t::duration_t {static_cast<double>(sample * 6)},
      1000,
      10,
      2,
      now
    );
  }

  const auto network = collector.snapshot().network;
  ASSERT_TRUE(network.valid);
  EXPECT_DOUBLE_EQ(network.send_queue.mean_ms, 50.5);
  EXPECT_DOUBLE_EQ(network.send_queue.p50_ms, 50.0);
  EXPECT_DOUBLE_EQ(network.send_queue.p95_ms, 95.0);
  EXPECT_DOUBLE_EQ(network.send_queue.p99_ms, 99.0);
  EXPECT_DOUBLE_EQ(network.packetization.mean_ms, 101.0);
  EXPECT_DOUBLE_EQ(network.fec.mean_ms, 151.5);
  EXPECT_DOUBLE_EQ(network.pacer.mean_ms, 202.0);
  EXPECT_DOUBLE_EQ(network.send.mean_ms, 252.5);
  EXPECT_DOUBLE_EQ(network.capture_to_last_send.mean_ms, 303.0);
  EXPECT_DOUBLE_EQ(network.wire_bitrate_kbps, 800.0);
  EXPECT_DOUBLE_EQ(network.fec_overhead_percent, 20.0);
  EXPECT_EQ(network.window_sequence, 1);
  EXPECT_DOUBLE_EQ(network.window_duration_ms, 1000.0);
  EXPECT_EQ(network.window_frames, 100);
  EXPECT_EQ(network.sampled_frames, 100);
  EXPECT_EQ(network.data_shards, 1000);
  EXPECT_EQ(network.fec_shards, 200);

  collector.record_network_frame(
    collector_t::duration_t {1.0},
    collector_t::duration_t {2.0},
    collector_t::duration_t {3.0},
    collector_t::duration_t {4.0},
    collector_t::duration_t {5.0},
    collector_t::duration_t {6.0},
    1000,
    10,
    2,
    start + std::chrono::seconds {2}
  );
  EXPECT_EQ(collector.snapshot().network.window_sequence, 2);
}

TEST(PipelineMetricsCollectorTest, BoundsSamplesAndTracksEveryFrame) {
  using collector_t = video::pipeline_metrics_collector_t;
  const collector_t::clock_t::time_point start {};
  collector_t collector {start};
  constexpr auto frame_count = collector_t::max_window_samples + 32;

  for (std::size_t sample = 0; sample < frame_count; ++sample) {
    const auto now =
      sample + 1 == frame_count ? start + collector_t::publish_interval :
                                  start + std::chrono::microseconds {static_cast<int64_t>(sample)};
    collector.record_frame(
      collector_t::duration_t {1.0},
      collector_t::duration_t {2.0},
      100,
      now
    );
  }

  const auto metrics = collector.snapshot();
  ASSERT_TRUE(metrics.valid);
  EXPECT_EQ(metrics.window_frames, frame_count);
  EXPECT_EQ(metrics.sampled_frames, collector_t::max_window_samples);
  EXPECT_EQ(metrics.frames_encoded, frame_count);

  for (std::size_t sample = 0; sample < frame_count; ++sample) {
    const auto now =
      sample + 1 == frame_count ? start + collector_t::publish_interval :
                                  start + std::chrono::microseconds {static_cast<int64_t>(sample)};
    collector.record_network_frame(
      collector_t::duration_t {1.0},
      collector_t::duration_t {2.0},
      collector_t::duration_t {3.0},
      collector_t::duration_t {4.0},
      collector_t::duration_t {5.0},
      collector_t::duration_t {6.0},
      100,
      10,
      2,
      now
    );
  }

  const auto network = collector.snapshot().network;
  ASSERT_TRUE(network.valid);
  EXPECT_EQ(network.window_frames, frame_count);
  EXPECT_EQ(network.sampled_frames, collector_t::max_window_samples);
}

TEST(PipelineMetricsCollectorTest, ResetClearsPublishedSessionState) {
  using collector_t = video::pipeline_metrics_collector_t;
  const collector_t::clock_t::time_point start {};
  collector_t collector {start};

  collector.record_drop();
  collector.record_frame(
    collector_t::duration_t {1.0},
    collector_t::duration_t {2.0},
    100,
    start + collector_t::publish_interval
  );
  ASSERT_TRUE(collector.snapshot().valid);

  collector.reset(start + std::chrono::seconds {2});
  auto metrics = collector.snapshot();
  EXPECT_FALSE(metrics.valid);
  EXPECT_EQ(metrics.window_sequence, 0);
  EXPECT_EQ(metrics.frames_encoded, 0);
  EXPECT_EQ(metrics.frames_dropped, 0);
  EXPECT_EQ(metrics.frames_replaced_before_encode, 0);
  EXPECT_EQ(metrics.frames_dropped_encode, 0);
  EXPECT_EQ(metrics.frames_dropped_encoded_queue, 0);
  EXPECT_EQ(metrics.frames_dropped_send_deadline, 0);
  EXPECT_EQ(metrics.frames_dropped_packet_deadline, 0);
  EXPECT_EQ(metrics.frames_dropped_recovery_wait, 0);
  EXPECT_EQ(metrics.frames_dropped_reference_superseded, 0);
  EXPECT_EQ(metrics.idr_requests_accepted, 0);
  EXPECT_EQ(metrics.idr_requests_rate_limited, 0);
  EXPECT_EQ(metrics.width, 0);
  EXPECT_EQ(metrics.height, 0);

  collector.record_network_frame(
    collector_t::duration_t {1.0},
    collector_t::duration_t {2.0},
    collector_t::duration_t {3.0},
    collector_t::duration_t {4.0},
    collector_t::duration_t {5.0},
    collector_t::duration_t {6.0},
    100,
    10,
    2,
    start + std::chrono::seconds {3}
  );
  ASSERT_TRUE(collector.snapshot().network.valid);

  collector.reset(start + std::chrono::seconds {4});
  metrics = collector.snapshot();
  EXPECT_FALSE(metrics.network.valid);
  EXPECT_EQ(metrics.network.window_sequence, 0);
}

TEST(PipelineMetricsRegistryTest, IsolatesConcurrentSessionTimelines) {
  using registry_t = video::pipeline_metrics_registry_t;
  const registry_t::clock_t::time_point start {};
  registry_t registry {start};
  constexpr registry_t::session_id_t first_session = 1;
  constexpr registry_t::session_id_t second_session = 2;

  ASSERT_TRUE(registry.register_session(first_session, start));
  ASSERT_TRUE(registry.register_session(second_session, start));
  ASSERT_TRUE(registry.set_resolution(first_session, 1920, 1080));
  ASSERT_TRUE(registry.set_resolution(second_session, 1280, 720));

  for (int sample = 1; sample <= 100; ++sample) {
    const auto now =
      sample == 100 ? start + video::pipeline_metrics_collector_t::publish_interval :
                      start + std::chrono::milliseconds {sample * 9};

    ASSERT_TRUE(registry.record_frame(
      first_session,
      registry_t::duration_t {static_cast<double>(sample)},
      registry_t::duration_t {static_cast<double>(sample * 2)},
      1000,
      now
    ));
    ASSERT_TRUE(registry.record_frame(
      second_session,
      registry_t::duration_t {static_cast<double>(sample * 10)},
      registry_t::duration_t {static_cast<double>(sample * 20)},
      2000,
      now
    ));
    ASSERT_TRUE(registry.record_network_frame(
      first_session,
      registry_t::duration_t {static_cast<double>(sample)},
      registry_t::duration_t {2.0},
      registry_t::duration_t {3.0},
      registry_t::duration_t {4.0},
      registry_t::duration_t {5.0},
      registry_t::duration_t {6.0},
      1200,
      10,
      2,
      now
    ));
    ASSERT_TRUE(registry.record_network_frame(
      second_session,
      registry_t::duration_t {static_cast<double>(sample * 10)},
      registry_t::duration_t {20.0},
      registry_t::duration_t {30.0},
      registry_t::duration_t {40.0},
      registry_t::duration_t {50.0},
      registry_t::duration_t {60.0},
      2400,
      10,
      4,
      now
    ));
  }

  const auto first = registry.snapshot(first_session);
  const auto second = registry.snapshot(second_session);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(first->valid);
  ASSERT_TRUE(second->valid);
  ASSERT_TRUE(first->network.valid);
  ASSERT_TRUE(second->network.valid);
  EXPECT_DOUBLE_EQ(first->encode_ms, 50.5);
  EXPECT_DOUBLE_EQ(second->encode_ms, 505.0);
  EXPECT_DOUBLE_EQ(first->network.send_queue.mean_ms, 50.5);
  EXPECT_DOUBLE_EQ(second->network.send_queue.mean_ms, 505.0);
  EXPECT_DOUBLE_EQ(first->network.fec_overhead_percent, 20.0);
  EXPECT_DOUBLE_EQ(second->network.fec_overhead_percent, 40.0);
  EXPECT_EQ(first->width, 1920);
  EXPECT_EQ(first->height, 1080);
  EXPECT_EQ(second->width, 1280);
  EXPECT_EQ(second->height, 720);

  const auto aggregate = registry.aggregate_snapshot();
  ASSERT_TRUE(aggregate.valid);
  EXPECT_EQ(aggregate.width, 0);
  EXPECT_EQ(aggregate.height, 0);
}

TEST(PipelineMetricsRegistryTest, RemovesEndedSessionsAndResetsNewCohort) {
  using registry_t = video::pipeline_metrics_registry_t;
  const registry_t::clock_t::time_point start {};
  registry_t registry {start};

  ASSERT_TRUE(registry.register_session(1, start));
  ASSERT_TRUE(registry.record_frame(
    1,
    registry_t::duration_t {1.0},
    registry_t::duration_t {2.0},
    100,
    start + std::chrono::seconds {1}
  ));
  ASSERT_TRUE(registry.aggregate_snapshot().valid);

  ASSERT_TRUE(registry.unregister_session(1));
  EXPECT_FALSE(registry.snapshot(1).has_value());
  EXPECT_EQ(registry.active_sessions(), 0);

  ASSERT_TRUE(registry.register_session(2, start + std::chrono::seconds {2}));
  EXPECT_FALSE(registry.aggregate_snapshot().valid);
  const auto second = registry.snapshot(2);
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(second->valid);
}

TEST(PipelineMetricsRegistryTest, EnforcesBoundedSessionCapacity) {
  using registry_t = video::pipeline_metrics_registry_t;
  const registry_t::clock_t::time_point start {};
  registry_t registry {start};

  for (std::size_t index = 0; index < registry_t::max_sessions; ++index) {
    ASSERT_TRUE(registry.register_session(index + 1, start));
  }
  EXPECT_EQ(registry.active_sessions(), registry_t::max_sessions);
  EXPECT_FALSE(registry.register_session(registry_t::max_sessions + 1, start));

  ASSERT_TRUE(registry.unregister_session(4));
  ASSERT_TRUE(registry.register_session(registry_t::max_sessions + 1, start));
  EXPECT_EQ(registry.active_sessions(), registry_t::max_sessions);

  EXPECT_FALSE(registry.record_drop(999));
  EXPECT_FALSE(registry.set_resolution(999, 1920, 1080));
  EXPECT_FALSE(registry.snapshot(999).has_value());
}

TEST(PipelineMetricsRegistryTest, AttributesQueueDropsToTheAffectedSession) {
  using registry_t = video::pipeline_metrics_registry_t;
  registry_t registry;

  ASSERT_TRUE(registry.register_session(1));
  ASSERT_TRUE(registry.register_session(2));
  ASSERT_TRUE(registry.record_drop(
    1,
    video::pipeline_drop_reason_e::encoded_queue,
    3
  ));
  ASSERT_TRUE(registry.record_drop(
    1,
    video::pipeline_drop_reason_e::packet_deadline,
    4
  ));
  ASSERT_TRUE(registry.record_drop(
    2,
    video::pipeline_drop_reason_e::capture_replaced,
    2
  ));

  const auto first = registry.snapshot(1);
  const auto second = registry.snapshot(2);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->frames_dropped, 7);
  EXPECT_EQ(first->frames_dropped_encoded_queue, 3);
  EXPECT_EQ(first->frames_dropped_packet_deadline, 4);
  EXPECT_EQ(first->frames_replaced_before_encode, 0);
  EXPECT_EQ(second->frames_dropped, 2);
  EXPECT_EQ(second->frames_dropped_encoded_queue, 0);
  EXPECT_EQ(second->frames_replaced_before_encode, 2);

  const auto aggregate = registry.aggregate_snapshot();
  EXPECT_EQ(aggregate.frames_dropped, 9);
  EXPECT_EQ(aggregate.frames_dropped_encoded_queue, 3);
  EXPECT_EQ(aggregate.frames_dropped_packet_deadline, 4);
  EXPECT_EQ(aggregate.frames_replaced_before_encode, 2);
}

TEST(PipelineMetricsRegistryTest, AttributesIdrGateCountersPerSession) {
  video::pipeline_metrics_registry_t registry;

  ASSERT_TRUE(registry.register_session(1));
  ASSERT_TRUE(registry.register_session(2));
  ASSERT_TRUE(registry.record_idr_request(1, true, 2));
  ASSERT_TRUE(registry.record_idr_request(1, false, 3));
  ASSERT_TRUE(registry.record_idr_request(2, true));

  const auto first = registry.snapshot(1);
  const auto second = registry.snapshot(2);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->idr_requests_accepted, 2);
  EXPECT_EQ(first->idr_requests_rate_limited, 3);
  EXPECT_EQ(second->idr_requests_accepted, 1);
  EXPECT_EQ(second->idr_requests_rate_limited, 0);

  const auto aggregate = registry.aggregate_snapshot();
  EXPECT_EQ(aggregate.idr_requests_accepted, 3);
  EXPECT_EQ(aggregate.idr_requests_rate_limited, 3);
  EXPECT_FALSE(registry.record_idr_request(999, true));
}

TEST(ThreadSafeEventTest, ReportsPendingValueReplacementAndStoppedRaises) {
  safe::event_t<int> event;

  const auto first = event.raise(1);
  EXPECT_TRUE(first.accepted);
  EXPECT_FALSE(first.replaced);

  const auto second = event.raise(2);
  EXPECT_TRUE(second.accepted);
  EXPECT_TRUE(second.replaced);

  const auto value = event.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 2);

  event.stop();
  const auto stopped = event.raise(3);
  EXPECT_FALSE(stopped.accepted);
  EXPECT_FALSE(stopped.replaced);
}

TEST(ThreadSafeQueueTest, ReportsAndInspectsBoundedBatchClears) {
  safe::queue_t<int> queue {2};
  queue.raise(1);
  queue.raise(2);

  const auto full_stats = queue.stats();
  EXPECT_TRUE(full_stats.running);
  EXPECT_EQ(full_stats.depth, 2);
  EXPECT_EQ(full_stats.capacity, 2);
  EXPECT_EQ(full_stats.high_watermark, 2);
  EXPECT_EQ(full_stats.overflow_events, 0);
  EXPECT_EQ(full_stats.dropped_elements, 0);

  std::vector<int> discarded;
  const auto result = queue.raise_with_overflow_handler(
    [&](int value) {
      discarded.push_back(value);
    },
    3
  );

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.dropped, 2);
  EXPECT_EQ(discarded, (std::vector<int> {1, 2}));

  const auto stats = queue.overflow_stats();
  EXPECT_EQ(stats.events, 1);
  EXPECT_EQ(stats.dropped_elements, 2);

  const auto overflowed_stats = queue.stats();
  EXPECT_EQ(overflowed_stats.depth, 1);
  EXPECT_EQ(overflowed_stats.capacity, 2);
  EXPECT_EQ(overflowed_stats.high_watermark, 2);
  EXPECT_EQ(overflowed_stats.overflow_events, 1);
  EXPECT_EQ(overflowed_stats.dropped_elements, 2);

  const auto value = queue.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 3);

  EXPECT_EQ(queue.stats().depth, 0);
  queue.stop();
  EXPECT_FALSE(queue.stats().running);
}

TEST(ThreadSafeQueueTest, PriorityInsertionPreservesUnmatchedFifoOrder) {
  safe::queue_t<int> queue {4};
  queue.raise(1);
  queue.raise(2);
  queue.raise(3);
  queue.raise(4);

  std::vector<int> superseded;
  const auto result = queue.raise_prioritized_with_cleanup(
    [](int value) {
      return value % 2 == 0;
    },
    [&](int value) {
      superseded.push_back(value);
    },
    [](int &) {},
    9
  );

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.superseded, 2);
  EXPECT_EQ(result.overflow_dropped, 0);
  EXPECT_EQ(superseded, (std::vector<int> {2, 4}));
  EXPECT_EQ(*queue.pop(), 9);
  EXPECT_EQ(*queue.pop(), 1);
  EXPECT_EQ(*queue.pop(), 3);
}

TEST(EncodedVideoQueueTest, OverflowMarksEveryAffectedSessionForRecovery) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets =
    mail->queue<video::packet_t>("encoded-video-overflow-test");
  int first_opaque_session;
  int second_opaque_session;
  auto *first_session = &first_opaque_session;
  auto *second_session = &second_opaque_session;
  auto &policy =
    stream::queueing::encoded_frame_queue_policy();
  policy.erase(first_session);
  policy.erase(second_session);

  const auto make_packet = [](void *session, int64_t frame_index) {
    auto packet = std::make_unique<video::packet_raw_generic>(
      std::vector<uint8_t> {0x01},
      frame_index,
      false
    );
    packet->channel_data = session;
    return packet;
  };

  for (int64_t frame = 0; frame < 32; ++frame) {
    video::enqueue_video_packet(
      packets,
      make_packet(
        frame % 2 == 0 ? first_session : second_session,
        frame
      )
    );
  }
  ASSERT_EQ(packets->stats().depth, 32);

  video::enqueue_video_packet(
    packets,
    make_packet(first_session, 32)
  );

  const auto first_decision = policy.evaluate({
    .session_key = first_session,
  });
  const auto second_decision = policy.evaluate({
    .session_key = second_session,
  });
  EXPECT_EQ(packets->stats().depth, 1);
  EXPECT_FALSE(first_decision.should_send());
  EXPECT_TRUE(first_decision.request_idr);
  EXPECT_FALSE(second_decision.should_send());
  EXPECT_TRUE(second_decision.request_idr);
  EXPECT_EQ(
    first_decision.recovery_cause,
    stream::queueing::frame_recovery_cause_e::
      encoded_queue_overflow
  );
  EXPECT_EQ(
    second_decision.recovery_cause,
    stream::queueing::frame_recovery_cause_e::
      encoded_queue_overflow
  );

  policy.erase(first_session);
  policy.erase(second_session);
}

TEST(EncodedVideoQueueTest, IdrSupersedesItsSessionAndMovesAhead) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto packets =
    mail->queue<video::packet_t>("encoded-video-priority-test");
  int first_opaque_session;
  int second_opaque_session;
  auto *first_session = &first_opaque_session;
  auto *second_session = &second_opaque_session;
  ASSERT_TRUE(video::metrics_register_session(first_session));

  const auto make_packet = [](
                             void *session,
                             int64_t frame_index,
                             bool is_idr = false
                           ) {
    auto packet = std::make_unique<video::packet_raw_generic>(
      std::vector<uint8_t> {0x01},
      frame_index,
      is_idr
    );
    packet->channel_data = session;
    return packet;
  };

  video::enqueue_video_packet(
    packets,
    make_packet(first_session, 1)
  );
  video::enqueue_video_packet(
    packets,
    make_packet(second_session, 2)
  );
  video::enqueue_video_packet(
    packets,
    make_packet(first_session, 3)
  );
  video::enqueue_video_packet(
    packets,
    make_packet(second_session, 4)
  );
  video::enqueue_video_packet(
    packets,
    make_packet(first_session, 5, true)
  );

  EXPECT_EQ(packets->stats().depth, 3);
  auto first = packets->pop();
  auto second = packets->pop();
  auto third = packets->pop();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  EXPECT_TRUE(first->is_idr());
  EXPECT_EQ(first->frame_index(), 5);
  EXPECT_EQ(second->frame_index(), 2);
  EXPECT_EQ(third->frame_index(), 4);

  const auto metrics =
    video::get_pipeline_metrics(first_session);
  ASSERT_TRUE(metrics.has_value());
  EXPECT_EQ(metrics->frames_dropped, 2);
  EXPECT_EQ(
    metrics->frames_dropped_reference_superseded,
    2
  );

  video::metrics_unregister_session(first_session);
}
