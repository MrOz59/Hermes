/**
 * @file tests/unit/test_congestion_controller.cpp
 * @brief Tests for the injectable Hermes congestion-control boundary.
 */

#include "../tests_common.h"

#include <array>
#include <chrono>
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
      true
    );
  EXPECT_EQ(enormous.pacing_bitrate_bps, 8'000'000);
  EXPECT_EQ(enormous.send_window, 250ms);
  EXPECT_TRUE(enormous.window_extended);
  EXPECT_TRUE(enormous.window_capped);
}
