/**
 * @file tests/unit/test_congestion_controller.cpp
 * @brief Tests for the injectable Hermes congestion-control boundary.
 */

#include "../tests_common.h"

#include <array>
#include <chrono>
#include <limits>
#include <src/congestion_controller.h>
#include <string_view>

using namespace std::literals;

namespace {

  class fake_congestion_controller_t final:
      public stream::congestion::ICongestionController {
  public:
    void on_packets_sent(
      const stream::congestion::sent_packet_batch_t &batch
    ) override {
      ++sent_calls;
      last_sent = batch;
    }

    void on_feedback(
      const stream::congestion::feedback_batch_t &feedback
    ) override {
      ++feedback_calls;
      last_legacy_loss = feedback.legacy_loss;
      feedback_received_at = feedback.received_at;
      if (!feedback.frame_reports.empty()) {
        last_frame_feedback = feedback.frame_reports.front();
      }
    }

    void on_path_changed(
      const stream::congestion::path_info_t &path
    ) override {
      ++path_calls;
      last_path = path;
    }

    [[nodiscard]] stream::congestion::congestion_target_t
      target() const override {
      return current_target;
    }

    stream::congestion::sent_packet_batch_t last_sent;
    stream::congestion::path_info_t last_path;
    stream::congestion::congestion_target_t current_target;
    std::optional<stream::congestion::legacy_loss_report_t>
      last_legacy_loss;
    std::optional<stream::congestion::frame_fec_feedback_t>
      last_frame_feedback;
    stream::congestion::congestion_time_point_t feedback_received_at {};
    int sent_calls = 0;
    int feedback_calls = 0;
    int path_calls = 0;
  };

}  // namespace

TEST(CongestionControllerTest, InterfaceSupportsFakeImplementation) {
  fake_congestion_controller_t fake;
  fake.current_target = {
    .encoder_bitrate_bps = 20'000'000,
    .pacing_bitrate_bps = 25'000'000,
    .fec_ratio_ppm = 200'000,
    .max_frame_queue_us = 8'000,
    .estimated_rtt_us = 2'000,
    .estimated_queue_delay_us = 500,
    .estimated_available_bitrate_bps = 30'000'000,
  };
  stream::congestion::ICongestionController &controller = fake;
  const auto now =
    stream::congestion::congestion_time_point_t {} + 7ms;
  const stream::congestion::frame_fec_feedback_t frame_feedback {
    .frame_id = 42,
    .missing_packets_before_highest_received = 2,
  };
  const std::array frame_reports {frame_feedback};

  controller.on_packets_sent({
    .frame_id = 42,
    .first_sequence_number = 100,
    .packet_count = 8,
    .data_packet_count = 6,
    .repair_packet_count = 2,
    .wire_bytes_per_packet = 1200,
    .is_key_frame = true,
    .sent_at = now,
  });
  controller.on_feedback({
    .frame_reports = frame_reports,
    .legacy_loss = std::nullopt,
    .received_at = now + 1ms,
  });
  controller.on_path_changed({
    .address_family =
      stream::congestion::path_address_family_e::ipv6,
    .remote_port = 48010,
    .maximum_datagram_size_bytes = 1200,
    .is_relayed = false,
    .observed_at = now,
  });

  const auto target = controller.target();
  EXPECT_EQ(fake.sent_calls, 1);
  EXPECT_EQ(fake.last_sent.frame_id, 42);
  EXPECT_EQ(fake.last_sent.packet_count, 8);
  EXPECT_EQ(fake.last_sent.data_packet_count, 6);
  EXPECT_EQ(fake.last_sent.repair_packet_count, 2);
  EXPECT_TRUE(fake.last_sent.is_key_frame);
  EXPECT_EQ(fake.feedback_calls, 1);
  ASSERT_TRUE(fake.last_frame_feedback);
  EXPECT_EQ(fake.last_frame_feedback->frame_id, 42);
  EXPECT_EQ(fake.feedback_received_at, now + 1ms);
  EXPECT_EQ(fake.path_calls, 1);
  EXPECT_EQ(
    fake.last_path.address_family,
    stream::congestion::path_address_family_e::ipv6
  );
  EXPECT_EQ(target.encoder_bitrate_bps, 20'000'000);
  EXPECT_EQ(target.pacing_bitrate_bps, 25'000'000);
  EXPECT_EQ(target.fec_ratio_ppm, 200'000);
  EXPECT_EQ(target.estimated_available_bitrate_bps, 30'000'000);
}

TEST(CongestionControllerTest, ParsesLegacyLossReportWithoutUnalignedReads) {
  constexpr std::array<std::uint8_t, 32> payload {
    0x04,
    0x03,
    0x02,
    0x01,  // Lost packets
    0xb8,
    0x0b,
    0x00,
    0x00,  // 3000 ms report interval
    0xe8,
    0x03,
    0x00,
    0x00,  // Legacy fixed field
    0x08,
    0x07,
    0x06,
    0x05,  // Last good frame, low half
    0x04,
    0x03,
    0x02,
    0x01,  // Last good frame, high half
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x14,
    0x00,
    0x00,
    0x00,
  };

  const auto report =
    stream::congestion::parse_gamestream_legacy_loss_report(
      std::string_view {
        reinterpret_cast<const char *>(payload.data()),
        payload.size(),
      }
    );

  ASSERT_TRUE(report);
  EXPECT_EQ(report->lost_packets, 0x01020304u);
  EXPECT_EQ(report->report_interval, 3000ms);
  EXPECT_EQ(report->last_good_frame, 0x0102030405060708ULL);
}

TEST(CongestionControllerTest, ParsesHestiaFrameFecFeedback) {
  constexpr std::array<std::uint8_t, 21> payload {
    0x01,
    0x02,
    0x03,
    0x04,  // Frame ID
    0x10,
    0x01,  // Highest received sequence
    0x10,
    0x02,  // Next contiguous sequence
    0x00,
    0x03,  // Missing packets
    0x00,
    0x20,  // Total data
    0x00,
    0x08,  // Total repair
    0x00,
    0x1e,  // Received data
    0x00,
    0x06,  // Received repair
    20,  // FEC percentage
    1,  // FEC block index
    4,  // FEC block count
  };

  const auto report =
    stream::congestion::parse_gamestream_frame_fec_feedback(
      std::string_view {
        reinterpret_cast<const char *>(payload.data()),
        payload.size(),
      }
    );

  ASSERT_TRUE(report);
  EXPECT_EQ(report->frame_id, 0x01020304u);
  EXPECT_EQ(report->highest_received_sequence_number, 0x1001);
  EXPECT_EQ(report->next_contiguous_sequence_number, 0x1002);
  EXPECT_EQ(report->missing_packets_before_highest_received, 3);
  EXPECT_EQ(report->total_data_packets, 32);
  EXPECT_EQ(report->total_repair_packets, 8);
  EXPECT_EQ(report->received_data_packets, 30);
  EXPECT_EQ(report->received_repair_packets, 6);
  EXPECT_EQ(report->fec_percentage, 20);
  EXPECT_EQ(report->fec_block_index, 1);
  EXPECT_EQ(report->fec_block_count, 4);
}

TEST(CongestionControllerTest, RejectsRuntGameStreamFeedback) {
  const std::array<char, 31> legacy_payload {};
  const std::array<char, 20> fec_payload {};

  EXPECT_FALSE(
    stream::congestion::parse_gamestream_legacy_loss_report(
      std::string_view {legacy_payload.data(), legacy_payload.size()}
    )
  );
  EXPECT_FALSE(
    stream::congestion::parse_gamestream_frame_fec_feedback(
      std::string_view {fec_payload.data(), fec_payload.size()}
    )
  );
}

TEST(CongestionControllerTest, LegacyControllerKeepsFixedTarget) {
  const stream::congestion::congestion_target_t expected {
    .encoder_bitrate_bps = 40'000'000,
    .pacing_bitrate_bps = 800'000'000,
    .fec_ratio_ppm = 200'000,
  };
  stream::congestion::legacy_fixed_congestion_controller_t controller {
    expected
  };
  const stream::congestion::frame_fec_feedback_t frame_feedback {
    .frame_id = 9,
    .missing_packets_before_highest_received = 4,
  };
  const std::array reports {frame_feedback};

  controller.on_packets_sent({
    .frame_id = 9,
    .packet_count = 12,
  });
  controller.on_feedback({
    .frame_reports = reports,
    .received_at =
      stream::congestion::congestion_clock_t::now(),
  });
  controller.on_path_changed({
    .address_family =
      stream::congestion::path_address_family_e::ipv4,
    .remote_port = 47998,
  });

  const auto actual = controller.target();
  EXPECT_EQ(actual.encoder_bitrate_bps, expected.encoder_bitrate_bps);
  EXPECT_EQ(actual.pacing_bitrate_bps, expected.pacing_bitrate_bps);
  EXPECT_EQ(actual.fec_ratio_ppm, expected.fec_ratio_ppm);
  EXPECT_EQ(actual.max_frame_queue_us, 0);
  EXPECT_EQ(actual.estimated_rtt_us, 0);
  EXPECT_EQ(actual.estimated_queue_delay_us, 0);
  EXPECT_EQ(actual.estimated_available_bitrate_bps, 0);
}

TEST(CongestionControllerTest, FixedPacingIncludesFecAndHeadroom) {
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_pacing_bitrate_bps(
      20'000'000,
      200'000
    ),
    26'400'000
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_pacing_bitrate_bps(
      100'000,
      0
    ),
    1'000'000
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_pacing_bitrate_bps(
      0,
      200'000
    ),
    stream::congestion::gamestream_pacing_ceiling_bps
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_pacing_bitrate_bps(
      790'000'000,
      1'000'000
    ),
    stream::congestion::gamestream_pacing_ceiling_bps
  );
}

TEST(CongestionControllerTest, DeadlinePacingFitsBoundedWindow) {
  EXPECT_EQ(
    stream::congestion::gamestream_deadline_pacing_bitrate_bps(
      8'000'000,
      100'000,
      40ms
    ),
    23'529'412
  );
  EXPECT_EQ(
    stream::congestion::gamestream_deadline_pacing_bitrate_bps(
      25'000'000,
      100'000,
      40ms
    ),
    25'000'000
  );
}

TEST(CongestionControllerTest, DeadlinePacingPreservesBounds) {
  EXPECT_EQ(
    stream::congestion::gamestream_deadline_pacing_bitrate_bps(
      12'000'000,
      0,
      40ms
    ),
    12'000'000
  );
  EXPECT_EQ(
    stream::congestion::gamestream_deadline_pacing_bitrate_bps(
      12'000'000,
      100'000,
      0us
    ),
    12'000'000
  );
  EXPECT_EQ(
    stream::congestion::gamestream_deadline_pacing_bitrate_bps(
      12'000'000,
      100'000'000,
      1ms
    ),
    stream::congestion::gamestream_pacing_ceiling_bps
  );
}

TEST(CongestionControllerTest, FixedQueueBudgetUsesTwoFrameIntervals) {
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_frame_queue_us(60),
    33'334
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_frame_queue_us(120),
    16'668
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_frame_queue_us(1000),
    8'000
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_frame_queue_us(5),
    100'000
  );
  EXPECT_EQ(
    stream::congestion::gamestream_fixed_frame_queue_us(0),
    50'000
  );
}

TEST(CongestionControllerTest, KeyFramesGetBoundedQueueAdmissionHeadroom) {
  EXPECT_EQ(
    stream::congestion::gamestream_frame_queue_budget(
      33'334,
      false
    ),
    33'334us
  );
  EXPECT_EQ(
    stream::congestion::gamestream_frame_queue_budget(
      33'334,
      true
    ),
    100ms
  );
  EXPECT_EQ(
    stream::congestion::gamestream_frame_queue_budget(
      16'668,
      true
    ),
    50'004us
  );
  EXPECT_EQ(
    stream::congestion::gamestream_frame_queue_budget(
      0,
      true
    ),
    0us
  );
}

TEST(CongestionControllerTest, RecoveryFrameExtendsImpossibleWindow) {
  constexpr std::uint64_t base_pacing_bitrate = 7'534'560;
  const auto plan =
    stream::congestion::gamestream_frame_pacing_plan(
      base_pacing_bitrate,
      434 * 1200,
      33'334us,
      0us,
      true
    );

  EXPECT_EQ(
    plan.pacing_bitrate_bps,
    base_pacing_bitrate * 8
  );
  EXPECT_GT(plan.send_window, 33'334us);
  EXPECT_LT(plan.send_window, 250ms);
  EXPECT_TRUE(plan.window_extended);
  EXPECT_FALSE(plan.window_capped);
}

TEST(CongestionControllerTest, NormalFrameKeepsNominalWindow) {
  const auto plan =
    stream::congestion::gamestream_frame_pacing_plan(
      8'000'000,
      20'000,
      33'334us,
      0us,
      false
    );

  EXPECT_EQ(plan.pacing_bitrate_bps, 8'000'000);
  EXPECT_EQ(plan.send_window, 33'334us);
  EXPECT_FALSE(plan.window_extended);
  EXPECT_FALSE(plan.window_capped);
}

TEST(CongestionControllerTest, FrameBurstAndWindowRemainBounded) {
  const auto high_bitrate =
    stream::congestion::gamestream_frame_pacing_plan(
      40'000'000,
      1'000'000,
      33'334us,
      0us,
      true
    );
  EXPECT_EQ(
    high_bitrate.pacing_bitrate_bps,
    stream::congestion::gamestream_frame_burst_ceiling_bps
  );

  const auto enormous =
    stream::congestion::gamestream_frame_pacing_plan(
      1'000'000,
      10'000'000,
      33'334us,
      0us,
      true
    );
  EXPECT_EQ(enormous.pacing_bitrate_bps, 8'000'000);
  EXPECT_EQ(enormous.send_window, 250ms);
  EXPECT_TRUE(enormous.window_extended);
  EXPECT_TRUE(enormous.window_capped);
}

TEST(CongestionControllerTest, QueuedNormalFrameUsesBoundedCatchUp) {
  constexpr std::uint64_t base_pacing_bitrate = 8'000'000;
  const auto plan =
    stream::congestion::gamestream_frame_pacing_plan(
      base_pacing_bitrate,
      20'000,
      33'334us,
      40ms,
      false
    );

  EXPECT_EQ(
    plan.pacing_bitrate_bps,
    base_pacing_bitrate * 4
  );
  EXPECT_EQ(plan.send_window, 33'334us);
  EXPECT_TRUE(plan.catch_up);
  EXPECT_FALSE(plan.window_capped);
}

TEST(CongestionControllerTest, QueueDelayRaisesRateWithoutCollapsingDeadline) {
  const auto plan =
    stream::congestion::gamestream_frame_pacing_plan(
      8'000'000,
      20'000,
      33'334us,
      20ms,
      false
    );

  EXPECT_GT(plan.pacing_bitrate_bps, 8'000'000);
  EXPECT_EQ(plan.send_window, 33'334us);
  EXPECT_TRUE(plan.catch_up);
}

// The absolute burst ceiling is a floor on available headroom, not a cap on
// the mechanism. A base rate above it must still scale, otherwise catch-up
// silently stops existing for every high-bitrate session.
TEST(CongestionControllerTest, BurstCeilingScalesWithBaseRate) {
  using stream::congestion::gamestream_frame_burst_ceiling;
  using stream::congestion::gamestream_frame_burst_ceiling_bps;
  using stream::congestion::gamestream_pacing_ceiling_bps;

  // Below the nominal ceiling, the scaled allowance is capped by it, so a
  // single frame cannot monopolize the link.
  EXPECT_EQ(
    gamestream_frame_burst_ceiling(10'000'000, 4),
    40'000'000u
  );
  EXPECT_EQ(
    gamestream_frame_burst_ceiling(50'000'000, 4),
    gamestream_frame_burst_ceiling_bps
  );
  // At or above it, the multiplier takes over: capping here would put the
  // ceiling at or below the base and disable catch-up outright.
  EXPECT_EQ(gamestream_frame_burst_ceiling(100'000'000, 4), 400'000'000u);
  EXPECT_EQ(gamestream_frame_burst_ceiling(150'000'000, 4), 600'000'000u);
  // Never above the legacy pacing ceiling, and never wrapping on a huge base.
  EXPECT_EQ(
    gamestream_frame_burst_ceiling(300'000'000, 8),
    gamestream_pacing_ceiling_bps
  );
  EXPECT_EQ(
    gamestream_frame_burst_ceiling(
      std::numeric_limits<std::uint64_t>::max(),
      8
    ),
    gamestream_pacing_ceiling_bps
  );
  // A zero multiplier must not divide by zero.
  EXPECT_EQ(
    gamestream_frame_burst_ceiling(50'000'000, 0),
    50'000'000u
  );
}

// Regression: at 150 Mbps the old flat ceiling collapsed the burst allowance
// back to the base rate, so a backlogged frame never caught up.
TEST(CongestionControllerTest, HighBitrateSessionStillCatchesUp) {
  constexpr std::uint64_t base_pacing_bitrate = 150'000'000;
  const auto plan =
    stream::congestion::gamestream_frame_pacing_plan(
      base_pacing_bitrate,
      400'000,
      16'667us,
      20ms,
      false
    );

  EXPECT_GT(plan.pacing_bitrate_bps, base_pacing_bitrate);
  EXPECT_TRUE(plan.catch_up);
}

namespace {

  stream::congestion::congestion_target_t adaptive_baseline() {
    return {
      .encoder_bitrate_bps = 20'000'000,
      .pacing_bitrate_bps =
        stream::congestion::gamestream_fixed_pacing_bitrate_bps(
          20'000'000,
          100'000
        ),
      .fec_ratio_ppm = 100'000,  // 10%
      .max_frame_queue_us = 33'334,
    };
  }

  /** @brief Deliver `frames` identical FEC reports at `now`. */
  void feed_frames(
    stream::congestion::adaptive_congestion_controller_t &controller,
    int frames,
    std::uint16_t data,
    std::uint16_t repair,
    std::uint16_t received_data,
    std::uint16_t received_repair,
    stream::congestion::congestion_time_point_t now
  ) {
    for (int i = 0; i < frames; ++i) {
      const std::array reports {
        stream::congestion::frame_fec_feedback_t {
          .total_data_packets = data,
          .total_repair_packets = repair,
          .received_data_packets = received_data,
          .received_repair_packets = received_repair,
        }
      };
      controller.on_feedback({
        .frame_reports = reports,
        .legacy_loss = std::nullopt,
        .received_at = now,
      });
    }
  }

}  // namespace

TEST(AdaptiveCongestionTest, HoldsBaselineWhileTheStreamIsClean) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  feed_frames(controller, 30, 10, 2, 10, 2, now);

  const auto target = controller.target();
  EXPECT_EQ(target.fec_ratio_ppm, adaptive_baseline().fec_ratio_ppm);
  EXPECT_EQ(target.pacing_bitrate_bps, adaptive_baseline().pacing_bitrate_bps);
}

// Loss the client repaired is invisible, so protection must not climb: this is
// the case that would otherwise cause constant oscillation.
TEST(AdaptiveCongestionTest, RecoveredLossDoesNotRaiseProtection) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  feed_frames(controller, 30, 10, 4, 7, 4, now);

  EXPECT_GT(controller.estimate().loss_ratio, 0.0);
  EXPECT_EQ(
    controller.target().fec_ratio_ppm,
    adaptive_baseline().fec_ratio_ppm
  );
}

TEST(AdaptiveCongestionTest, RaisesProtectionOnUnrecoverableLoss) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  feed_frames(controller, 30, 10, 2, 5, 2, now);

  const auto target = controller.target();
  EXPECT_GT(target.fec_ratio_ppm, adaptive_baseline().fec_ratio_ppm);
  // Pacing rises to carry the extra repair shards.
  EXPECT_GT(target.pacing_bitrate_bps, adaptive_baseline().pacing_bitrate_bps);
}

// The encoder rate is fixed once configured, so lowering pacing would queue
// frames into their deadline instead of relieving congestion.
TEST(AdaptiveCongestionTest, NeverPacesBelowTheConfiguredBaseline) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};

  for (int round = 0; round < 10; ++round) {
    feed_frames(controller, 20, 10, 2, 5, 2, now);
    now += 500ms;
    feed_frames(controller, 20, 10, 2, 10, 2, now);
    now += 500ms;
    EXPECT_GE(
      controller.target().pacing_bitrate_bps,
      adaptive_baseline().pacing_bitrate_bps
    );
    EXPECT_GE(
      controller.target().fec_ratio_ppm,
      adaptive_baseline().fec_ratio_ppm
    );
  }
}

TEST(AdaptiveCongestionTest, BoundsProtectionUnderSustainedLoss) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};

  for (int round = 0; round < 40; ++round) {
    feed_frames(controller, 20, 10, 2, 1, 0, now);
    now += 100ms;
  }

  EXPECT_LE(
    controller.target().fec_ratio_ppm,
    stream::congestion::adaptive_congestion_controller_t::
      maximum_fec_ratio_ppm
  );
}

// H3 requires releasing protection without flapping: the hold-down keeps a
// single clean window from immediately undoing a raise.
TEST(AdaptiveCongestionTest, ReleasesProtectionOnlyAfterHoldDown) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};

  feed_frames(controller, 20, 10, 2, 5, 2, now);
  const auto raised = controller.target().fec_ratio_ppm;
  ASSERT_GT(raised, adaptive_baseline().fec_ratio_ppm);

  // Clean feedback arrives immediately: too soon to release.
  now += 100ms;
  feed_frames(controller, 20, 10, 2, 10, 2, now);
  EXPECT_EQ(controller.target().fec_ratio_ppm, raised);

  // After the hold-down, protection steps back down.
  now += stream::congestion::adaptive_congestion_controller_t::
           release_hold_down +
         100ms;
  feed_frames(controller, 20, 10, 2, 10, 2, now);
  EXPECT_LT(controller.target().fec_ratio_ppm, raised);
}

TEST(AdaptiveCongestionTest, ReleaseStopsAtTheConfiguredBaseline) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};

  feed_frames(controller, 20, 10, 2, 5, 2, now);
  for (int round = 0; round < 20; ++round) {
    now += stream::congestion::adaptive_congestion_controller_t::
             release_hold_down +
           100ms;
    feed_frames(controller, 20, 10, 2, 10, 2, now);
  }

  EXPECT_EQ(
    controller.target().fec_ratio_ppm,
    adaptive_baseline().fec_ratio_ppm
  );
}

// A path change invalidates everything measured about the old link.
TEST(AdaptiveCongestionTest, PathChangeReturnsToBaseline) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  feed_frames(controller, 20, 10, 2, 5, 2, now);
  ASSERT_GT(controller.target().fec_ratio_ppm, adaptive_baseline().fec_ratio_ppm);

  controller.on_path_changed({.observed_at = now + 1s});

  EXPECT_EQ(
    controller.target().fec_ratio_ppm,
    adaptive_baseline().fec_ratio_ppm
  );
  EXPECT_FALSE(controller.estimate().valid);
}

// fec_percentage accepts 1..255 and now round-trips through the target back
// into the broadcaster, so a high configuration must survive intact.
TEST(AdaptiveCongestionTest, HighConfiguredProtectionIsNeverLowered) {
  auto baseline = adaptive_baseline();
  baseline.fec_ratio_ppm = 2'550'000;  // 255%, above the adaptive ceiling
  stream::congestion::adaptive_congestion_controller_t controller {baseline};
  auto now = stream::congestion::congestion_time_point_t {};

  // Sustained unrecoverable loss: raising must not clamp down to the ceiling.
  for (int round = 0; round < 5; ++round) {
    feed_frames(controller, 20, 10, 2, 1, 0, now);
    now += 100ms;
  }
  EXPECT_GE(controller.target().fec_ratio_ppm, baseline.fec_ratio_ppm);

  // Releasing must also stop at the configured level, not the ceiling.
  for (int round = 0; round < 20; ++round) {
    now += stream::congestion::adaptive_congestion_controller_t::
             release_hold_down +
           100ms;
    feed_frames(controller, 20, 10, 2, 10, 2, now);
  }
  EXPECT_EQ(controller.target().fec_ratio_ppm, baseline.fec_ratio_ppm);
}

// With adaptive FEC off the broadcaster must see exactly the configured
// percentage, since it now derives it from the target rather than the config.
TEST(CongestionControllerTest, FixedControllerRoundTripsFecPercentage) {
  for (const int percentage : {1, 10, 20, 100, 155, 255}) {
    const auto ppm = static_cast<std::uint32_t>(percentage) * 10'000;
    stream::congestion::legacy_fixed_congestion_controller_t controller {
      {.fec_ratio_ppm = ppm}
    };
    EXPECT_EQ(
      static_cast<int>(controller.target().fec_ratio_ppm / 10'000),
      percentage
    );
  }
}

// A lost key frame stalls the picture until a replacement is requested,
// encoded and delivered, so it is protected above normal frames from the first
// frame of the session rather than only after loss has been measured.
TEST(AdaptiveCongestionTest, ProtectsKeyFramesAboveNormalFramesFromTheStart) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };

  const auto target = controller.target();
  EXPECT_EQ(target.fec_ratio_ppm, adaptive_baseline().fec_ratio_ppm);
  EXPECT_GT(
    stream::congestion::frame_fec_ratio_ppm(target, true),
    stream::congestion::frame_fec_ratio_ppm(target, false)
  );
}

TEST(AdaptiveCongestionTest, KeyFrameProtectionFollowsTheAdaptiveLevel) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};
  const auto before =
    controller.target().key_frame_fec_ratio_ppm;

  feed_frames(controller, 30, 10, 2, 5, 2, now);

  const auto target = controller.target();
  EXPECT_GT(target.key_frame_fec_ratio_ppm, before);
  EXPECT_GT(target.key_frame_fec_ratio_ppm, target.fec_ratio_ppm);
}

TEST(AdaptiveCongestionTest, BoundsKeyFrameProtectionUnderSustainedLoss) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};

  for (int round = 0; round < 40; ++round) {
    feed_frames(controller, 20, 10, 2, 1, 0, now);
    now += 100ms;
  }

  EXPECT_LE(
    controller.target().key_frame_fec_ratio_ppm,
    stream::congestion::adaptive_congestion_controller_t::
      maximum_key_frame_fec_ratio_ppm
  );
}

// A host configuring protection above the adaptive ceiling must not end up
// protecting its key frames less than everything else.
TEST(AdaptiveCongestionTest, KeyFrameProtectionNeverFallsBelowNormalFrames) {
  auto baseline = adaptive_baseline();
  baseline.fec_ratio_ppm = 2'550'000;  // 255%, above every adaptive ceiling
  stream::congestion::adaptive_congestion_controller_t controller {baseline};
  auto now = stream::congestion::congestion_time_point_t {};

  for (int round = 0; round < 5; ++round) {
    feed_frames(controller, 20, 10, 2, 1, 0, now);
    now += 100ms;
  }

  const auto target = controller.target();
  EXPECT_GE(target.key_frame_fec_ratio_ppm, target.fec_ratio_ppm);
  EXPECT_EQ(
    stream::congestion::frame_fec_ratio_ppm(target, true),
    target.fec_ratio_ppm
  );
}

TEST(AdaptiveCongestionTest, PathChangeReturnsKeyFrameProtectionToBaseline) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};
  const auto baseline_key_frame_ppm =
    controller.target().key_frame_fec_ratio_ppm;

  feed_frames(controller, 20, 10, 2, 5, 2, now);
  ASSERT_GT(
    controller.target().key_frame_fec_ratio_ppm,
    baseline_key_frame_ppm
  );

  controller.on_path_changed({.observed_at = now + 1s});

  EXPECT_EQ(
    controller.target().key_frame_fec_ratio_ppm,
    baseline_key_frame_ppm
  );
}

// With adaptive FEC off, key frames must use exactly the configured
// percentage: the frame-type policy ships with the experimental flag.
TEST(CongestionControllerTest, FixedControllerTreatsEveryFrameTypeAlike) {
  stream::congestion::legacy_fixed_congestion_controller_t controller {
    {.fec_ratio_ppm = 200'000}
  };

  const auto target = controller.target();
  EXPECT_EQ(
    stream::congestion::frame_fec_ratio_ppm(target, true),
    target.fec_ratio_ppm
  );
  EXPECT_EQ(
    stream::congestion::frame_fec_ratio_ppm(target, false),
    target.fec_ratio_ppm
  );
}

// The capacity estimate is advisory, so its whole value is being trustworthy:
// a session that never loses anything must keep reporting exactly what the
// encoder was configured for.
TEST(AdaptiveCongestionTest, ReportsTheConfiguredBitrateWhileTheStreamIsClean) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  EXPECT_EQ(
    controller.target().estimated_available_bitrate_bps,
    adaptive_baseline().encoder_bitrate_bps
  );

  feed_frames(controller, 30, 10, 2, 10, 2, now);

  EXPECT_EQ(
    controller.target().estimated_available_bitrate_bps,
    adaptive_baseline().encoder_bitrate_bps
  );
}

TEST(AdaptiveCongestionTest, LowersTheBitrateEstimateOnUnrecoverableLoss) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};
  const auto configured = adaptive_baseline().encoder_bitrate_bps;

  // The hold-down has not elapsed yet, so the first lossy window only moves
  // protection: capacity claims wait for evidence that outlives one window.
  feed_frames(controller, 30, 10, 2, 5, 2, now);
  EXPECT_EQ(
    controller.target().estimated_available_bitrate_bps,
    configured
  );

  feed_frames(
    controller,
    30,
    10,
    2,
    5,
    2,
    now +
      stream::congestion::adaptive_congestion_controller_t::bitrate_hold_down
  );

  const auto lowered =
    controller.target().estimated_available_bitrate_bps;
  EXPECT_LT(lowered, configured);
  // One multiplicative step, not one per report in the batch.
  EXPECT_EQ(
    lowered,
    configured *
      stream::congestion::adaptive_congestion_controller_t::
        bitrate_decrease_permille /
      1000
  );
}

TEST(AdaptiveCongestionTest, NeverEstimatesBelowTheConfiguredFloor) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};
  const auto configured = adaptive_baseline().encoder_bitrate_bps;
  const auto floor_bps =
    configured *
    stream::congestion::adaptive_congestion_controller_t::
      minimum_bitrate_permille /
    1000;

  for (int round = 0; round < 20; ++round) {
    feed_frames(controller, 20, 10, 2, 3, 1, now);
    now +=
      stream::congestion::adaptive_congestion_controller_t::
        bitrate_hold_down;
  }

  EXPECT_EQ(
    controller.target().estimated_available_bitrate_bps,
    floor_bps
  );
}

TEST(AdaptiveCongestionTest, RecoversTheBitrateEstimateInSteps) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};
  const auto configured = adaptive_baseline().encoder_bitrate_bps;
  const auto hold_down =
    stream::congestion::adaptive_congestion_controller_t::bitrate_hold_down;

  feed_frames(controller, 20, 10, 2, 5, 2, now);
  now += hold_down;
  feed_frames(controller, 20, 10, 2, 5, 2, now);
  const auto lowered =
    controller.target().estimated_available_bitrate_bps;
  ASSERT_LT(lowered, configured);

  // A link that just failed is probed, not trusted: one clean window gives
  // back a step, not everything it had failed to carry.
  now += hold_down;
  feed_frames(controller, 20, 10, 2, 10, 2, now);
  const auto probing =
    controller.target().estimated_available_bitrate_bps;
  EXPECT_GT(probing, lowered);
  EXPECT_LT(probing, configured);

  for (int round = 0; round < 10; ++round) {
    now += hold_down;
    feed_frames(controller, 20, 10, 2, 10, 2, now);
  }

  // Recovery stops exactly at what the encoder produces. Reporting more would
  // describe capacity this controller has no way to measure.
  EXPECT_EQ(
    controller.target().estimated_available_bitrate_bps,
    configured
  );
}

TEST(AdaptiveCongestionTest, PathChangeReturnsTheBitrateEstimateToBaseline) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};
  const auto configured = adaptive_baseline().encoder_bitrate_bps;

  feed_frames(controller, 20, 10, 2, 5, 2, now);
  now +=
    stream::congestion::adaptive_congestion_controller_t::bitrate_hold_down;
  feed_frames(controller, 20, 10, 2, 5, 2, now);
  ASSERT_LT(
    controller.target().estimated_available_bitrate_bps,
    configured
  );

  controller.on_path_changed({.observed_at = now + 1s});

  EXPECT_EQ(
    controller.target().estimated_available_bitrate_bps,
    configured
  );
}

// Diagnostics decide "not adapting" from the estimate's validity, so the fixed
// controller must not answer as if it had measured a healthy link.
TEST(CongestionControllerTest, FixedControllerReportsNoEstimate) {
  stream::congestion::legacy_fixed_congestion_controller_t controller {
    {.encoder_bitrate_bps = 20'000'000}
  };
  stream::congestion::ICongestionController &boundary = controller;

  EXPECT_FALSE(boundary.estimate().valid);
}

TEST(AdaptiveCongestionTest, DerivesQueueDelayFromTheSmallestRoundTrip) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  controller.on_rtt_sample(20ms, now);
  EXPECT_EQ(controller.target().estimated_rtt_us, 20'000u);
  // The first sample is its own baseline, so nothing is queued yet.
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 0u);

  controller.on_rtt_sample(35ms, now + 1s);
  EXPECT_EQ(controller.target().estimated_rtt_us, 35'000u);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 15'000u);

  // A lower round trip is a better view of the path itself, so it replaces the
  // baseline immediately rather than waiting for it to expire.
  controller.on_rtt_sample(12ms, now + 2s);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 0u);
  controller.on_rtt_sample(20ms, now + 3s);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 8'000u);
}

// A path whose propagation delay grew must not be measured forever against a
// minimum it can no longer reach, or every sample reads as queueing.
TEST(AdaptiveCongestionTest, ExpiresTheRoundTripBaseline) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};
  const auto window =
    stream::congestion::adaptive_congestion_controller_t::minimum_rtt_window;

  controller.on_rtt_sample(10ms, now);
  controller.on_rtt_sample(60ms, now + 1s);
  ASSERT_EQ(controller.target().estimated_queue_delay_us, 50'000u);

  controller.on_rtt_sample(60ms, now + window + 2s);

  EXPECT_EQ(controller.target().estimated_rtt_us, 60'000u);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 0u);
}

TEST(AdaptiveCongestionTest, IgnoresRoundTripSamplesTheTransportHasNotMeasured) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  controller.on_rtt_sample(std::chrono::microseconds {0}, now);

  EXPECT_EQ(controller.target().estimated_rtt_us, 0u);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 0u);
}

TEST(AdaptiveCongestionTest, PathChangeRebaselinesTheRoundTrip) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  controller.on_rtt_sample(10ms, now);
  controller.on_rtt_sample(40ms, now + 1s);
  ASSERT_GT(controller.target().estimated_queue_delay_us, 0u);

  controller.on_path_changed({.observed_at = now + 2s});
  EXPECT_EQ(controller.target().estimated_rtt_us, 0u);

  // The first sample on the new path is the new baseline, not queueing
  // inherited from the old one.
  controller.on_rtt_sample(40ms, now + 3s);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 0u);
}

// The fixed controller models no delay, so the boundary's default must be a
// no-op rather than something that publishes a target it does not maintain.
TEST(CongestionControllerTest, FixedControllerIgnoresRoundTripSamples) {
  stream::congestion::legacy_fixed_congestion_controller_t controller {
    {.fec_ratio_ppm = 100'000}
  };
  stream::congestion::ICongestionController &boundary = controller;

  boundary.on_rtt_sample(
    25ms,
    stream::congestion::congestion_time_point_t {}
  );

  EXPECT_EQ(controller.target().estimated_rtt_us, 0u);
  EXPECT_EQ(controller.target().estimated_queue_delay_us, 0u);
}

namespace {

  /** @brief Drive the round trip to a queue-congested state and back. */
  void feed_rtt(
    stream::congestion::adaptive_congestion_controller_t &controller,
    std::chrono::milliseconds baseline_rtt,
    std::chrono::milliseconds current_rtt,
    stream::congestion::congestion_time_point_t now
  ) {
    controller.on_rtt_sample(baseline_rtt, now);
    controller.on_rtt_sample(current_rtt, now);
  }

}  // namespace

// A filling queue says the path is past its capacity before the buffer
// overflows into loss the client can report, which is the whole reason to
// watch delay at all.
TEST(AdaptiveCongestionTest, QueueDelayAloneLowersTheBitrateEstimate) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};
  const auto configured = adaptive_baseline().encoder_bitrate_bps;
  const auto hold_down =
    stream::congestion::adaptive_congestion_controller_t::bitrate_hold_down;

  feed_rtt(controller, 10ms, 80ms, now);
  ASSERT_TRUE(controller.queue_congested());
  // No client feedback at all: delay is measured independently of it.
  ASSERT_FALSE(controller.estimate().valid);

  controller.on_rtt_sample(80ms, now + hold_down);

  EXPECT_LT(
    controller.target().estimated_available_bitrate_bps,
    configured
  );
}

// Repair shards are bytes, and bytes are what fill a queue. Adding them to a
// path that is already queueing would deepen it and drop the data shards the
// repair was meant to protect.
TEST(AdaptiveCongestionTest, HoldsProtectionWhileTheQueueIsDeep) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  feed_rtt(controller, 10ms, 90ms, now);
  ASSERT_TRUE(controller.queue_congested());

  feed_frames(controller, 30, 10, 2, 5, 2, now);

  EXPECT_EQ(
    controller.target().fec_ratio_ppm,
    adaptive_baseline().fec_ratio_ppm
  );
}

// The same loss with a drained queue is link loss, and repair is exactly the
// right answer for it.
TEST(AdaptiveCongestionTest, RaisesProtectionForLossOnADrainedPath) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  feed_rtt(controller, 10ms, 12ms, now);
  ASSERT_FALSE(controller.queue_congested());

  feed_frames(controller, 30, 10, 2, 5, 2, now);

  EXPECT_GT(
    controller.target().fec_ratio_ppm,
    adaptive_baseline().fec_ratio_ppm
  );
}

TEST(AdaptiveCongestionTest, QueueStateUsesHysteresis) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  const auto now = stream::congestion::congestion_time_point_t {};

  controller.on_rtt_sample(10ms, now);
  controller.on_rtt_sample(50ms, now);
  ASSERT_TRUE(controller.queue_congested());

  // Between the two thresholds the previous conclusion stands, so a path
  // hovering near one of them cannot flip state on every sample.
  controller.on_rtt_sample(30ms, now + 1s);
  EXPECT_TRUE(controller.queue_congested());

  controller.on_rtt_sample(15ms, now + 2s);
  EXPECT_FALSE(controller.queue_congested());
}

// Recovery needs positive evidence from both signals: a drained queue while
// the client still reports unrepaired loss is not a recovered path.
TEST(AdaptiveCongestionTest, DoesNotRecoverBitrateWhileTheQueueIsDeep) {
  stream::congestion::adaptive_congestion_controller_t controller {
    adaptive_baseline()
  };
  auto now = stream::congestion::congestion_time_point_t {};
  const auto hold_down =
    stream::congestion::adaptive_congestion_controller_t::bitrate_hold_down;

  feed_rtt(controller, 10ms, 90ms, now);
  now += hold_down;
  controller.on_rtt_sample(90ms, now);
  const auto lowered =
    controller.target().estimated_available_bitrate_bps;
  ASSERT_LT(lowered, adaptive_baseline().encoder_bitrate_bps);

  // Clean frames, but the queue is still deep.
  now += hold_down;
  feed_frames(controller, 20, 10, 2, 10, 2, now);

  EXPECT_LE(
    controller.target().estimated_available_bitrate_bps,
    lowered
  );
}
