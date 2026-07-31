/**
 * @file tests/unit/test_packet_feedback.cpp
 * @brief Tests for per-packet feedback parsing and the estimates from it.
 */

#include "../tests_common.h"

#include <array>
#include <chrono>
#include <src/packet_feedback.h>
#include <string>
#include <vector>

using namespace std::literals;

namespace {

  namespace cg = stream::congestion;

  void append_big16(std::string &payload, std::uint16_t value) {
    payload.push_back(static_cast<char>((value >> 8) & 0xff));
    payload.push_back(static_cast<char>(value & 0xff));
  }

  void append_big32(std::string &payload, std::uint32_t value) {
    append_big16(payload, static_cast<std::uint16_t>(value >> 16));
    append_big16(payload, static_cast<std::uint16_t>(value & 0xffff));
  }

  /** @brief Build a v1 report; a negative arrival marks a lost packet. */
  std::string build_report(
    std::uint16_t report_sequence,
    std::uint16_t base_sequence,
    std::uint32_t reference_time_us,
    const std::vector<int> &arrival_offsets_us
  ) {
    std::string payload;
    append_big16(payload, report_sequence);
    append_big16(payload, base_sequence);
    append_big16(payload, static_cast<std::uint16_t>(arrival_offsets_us.size()));
    append_big32(payload, reference_time_us);
    for (const auto offset_us : arrival_offsets_us) {
      if (offset_us < 0) {
        append_big16(payload, 0);
        continue;
      }
      const auto units = static_cast<std::uint16_t>(
        offset_us / static_cast<int>(cg::packet_feedback_time_unit_us)
      );
      append_big16(payload, static_cast<std::uint16_t>(0x8000 | units));
    }
    return payload;
  }

}  // namespace

TEST(PacketFeedbackTest, ParsesReceivedAndLostMetrics) {
  std::array<cg::packet_metric_t, 8> storage {};
  const auto payload = build_report(7, 1000, 500'000, {0, -1, 128, 192});

  const auto report = cg::parse_packet_feedback(payload, storage);

  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->report_sequence, 7);
  EXPECT_EQ(report->base_sequence, 1000);
  EXPECT_EQ(report->reference_time, 500'000us);
  ASSERT_EQ(report->metrics.size(), 4u);

  EXPECT_EQ(report->metrics[0].sequence_number, 1000);
  EXPECT_TRUE(report->metrics[0].received);
  EXPECT_EQ(report->metrics[0].arrival_offset, 0us);

  EXPECT_EQ(report->metrics[1].sequence_number, 1001);
  EXPECT_FALSE(report->metrics[1].received);

  EXPECT_EQ(report->metrics[2].sequence_number, 1002);
  EXPECT_TRUE(report->metrics[2].received);
  EXPECT_EQ(report->metrics[2].arrival_offset, 128us);
  EXPECT_EQ(report->metrics[3].arrival_offset, 192us);
}

// Sequence numbers wrap, and a report spanning the wrap is ordinary.
TEST(PacketFeedbackTest, SequenceNumbersWrapWithinAReport) {
  std::array<cg::packet_metric_t, 4> storage {};
  const auto payload = build_report(1, 65534, 0, {0, 64, 128, 192});

  const auto report = cg::parse_packet_feedback(payload, storage);

  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->metrics[0].sequence_number, 65534);
  EXPECT_EQ(report->metrics[1].sequence_number, 65535);
  EXPECT_EQ(report->metrics[2].sequence_number, 0);
  EXPECT_EQ(report->metrics[3].sequence_number, 1);
}

// A count that does not match the payload means the message was truncated or
// forged; reading it anyway would read whatever follows in memory.
TEST(PacketFeedbackTest, RejectsPayloadsThatDoNotMatchTheirCount) {
  std::array<cg::packet_metric_t, 8> storage {};

  auto truncated = build_report(1, 100, 0, {0, 64, 128});
  truncated.resize(truncated.size() - 2);
  EXPECT_FALSE(cg::parse_packet_feedback(truncated, storage).has_value());

  auto padded = build_report(1, 100, 0, {0, 64, 128});
  padded.push_back('\0');
  padded.push_back('\0');
  EXPECT_FALSE(cg::parse_packet_feedback(padded, storage).has_value());

  EXPECT_FALSE(cg::parse_packet_feedback("short", storage).has_value());
  EXPECT_FALSE(cg::parse_packet_feedback(build_report(1, 100, 0, {}), storage).has_value());
}

TEST(PacketFeedbackTest, RejectsReportsLargerThanTheCallersStorage) {
  std::array<cg::packet_metric_t, 2> storage {};
  const auto payload = build_report(1, 100, 0, {0, 64, 128, 192});

  EXPECT_FALSE(cg::parse_packet_feedback(payload, storage).has_value());
}

TEST(PacketFeedbackTest, HistoryMatchesOnlyTheSequenceItHolds) {
  cg::packet_send_history_t history;
  const cg::feedback_time_point_t now {};

  history.record(500, 3, 1200, now);

  EXPECT_TRUE(history.find(500).known);
  EXPECT_EQ(history.find(500).wire_bytes, 1200u);
  EXPECT_TRUE(history.find(502).known);
  EXPECT_FALSE(history.find(503).known);

  // A slot reused by a later sequence number must not answer for the old one.
  history.record(
    static_cast<std::uint16_t>(500 + cg::packet_send_history_t::capacity),
    1,
    900,
    now + 1s
  );
  EXPECT_FALSE(history.find(500).known);

  history.reset();
  EXPECT_FALSE(history.find(501).known);
}

TEST(PacketFeedbackTest, MeasuresDeliveryRateFromArrivalTimes) {
  cg::packet_send_history_t history;
  const cg::feedback_time_point_t start {};
  std::array<cg::packet_metric_t, 8> storage {};

  // Five packets of 1000 wire bytes, sent 1 ms apart.
  for (std::uint16_t index = 0; index < 5; ++index) {
    history.record(
      static_cast<std::uint16_t>(200 + index),
      1,
      1000,
      start + std::chrono::milliseconds {index}
    );
  }

  // All arrived, 1 ms apart: the path kept up exactly.
  const auto payload = build_report(1, 200, 0, {0, 1024, 2048, 3072, 4032});
  const auto report = cg::parse_packet_feedback(payload, storage);
  ASSERT_TRUE(report.has_value());

  const auto estimate = cg::analyze_packet_feedback(*report, history);

  ASSERT_TRUE(estimate.valid);
  EXPECT_EQ(estimate.delivered_packets, 5u);
  EXPECT_EQ(estimate.lost_packets, 0u);
  // Four packets of 1000 bytes across ~4.03 ms of arrivals.
  EXPECT_GT(estimate.delivery_rate_bps, 7'000'000u);
  EXPECT_LT(estimate.delivery_rate_bps, 8'500'000u);
}

// The gradient is what sees a queue forming: arrivals spreading out further
// than departures did.
TEST(PacketFeedbackTest, PositiveGradientWhenArrivalsSpreadOut) {
  cg::packet_send_history_t history;
  const cg::feedback_time_point_t start {};
  std::array<cg::packet_metric_t, 8> storage {};

  for (std::uint16_t index = 0; index < 4; ++index) {
    history.record(
      static_cast<std::uint16_t>(10 + index),
      1,
      1200,
      start + std::chrono::milliseconds {index}
    );
  }

  // Sent across 3 ms, arrived across ~12 ms.
  const auto payload = build_report(1, 10, 0, {0, 4032, 8000, 11968});
  const auto report = cg::parse_packet_feedback(payload, storage);
  ASSERT_TRUE(report.has_value());

  const auto estimate = cg::analyze_packet_feedback(*report, history);

  ASSERT_TRUE(estimate.valid);
  EXPECT_GT(estimate.delay_gradient_us, 8'000);
}

TEST(PacketFeedbackTest, NegativeGradientWhenAQueueDrains) {
  cg::packet_send_history_t history;
  const cg::feedback_time_point_t start {};
  std::array<cg::packet_metric_t, 8> storage {};

  for (std::uint16_t index = 0; index < 4; ++index) {
    history.record(
      static_cast<std::uint16_t>(10 + index),
      1,
      1200,
      start + std::chrono::milliseconds {index * 4}
    );
  }

  // Sent across 12 ms, arrived across ~4 ms: the backlog is draining.
  const auto payload = build_report(1, 10, 0, {0, 1344, 2688, 4032});
  const auto report = cg::parse_packet_feedback(payload, storage);
  ASSERT_TRUE(report.has_value());

  const auto estimate = cg::analyze_packet_feedback(*report, history);

  EXPECT_LT(estimate.delay_gradient_us, 0);
}

// Acknowledged packets whose record is gone carry no size or departure, so
// they cannot contribute to a rate without inventing both.
TEST(PacketFeedbackTest, SkipsAcknowledgedPacketsItNoLongerRemembers) {
  cg::packet_send_history_t history;
  const cg::feedback_time_point_t start {};
  std::array<cg::packet_metric_t, 8> storage {};

  history.record(300, 1, 1000, start);
  history.record(302, 1, 1000, start + 2ms);

  const auto payload = build_report(1, 300, 0, {0, 1024, 2048});
  const auto report = cg::parse_packet_feedback(payload, storage);
  ASSERT_TRUE(report.has_value());

  const auto estimate = cg::analyze_packet_feedback(*report, history);

  EXPECT_EQ(estimate.delivered_packets, 2u);
  EXPECT_EQ(estimate.lost_packets, 0u);
}

TEST(PacketFeedbackTest, CountsLostPacketsWithoutConcludingARate) {
  cg::packet_send_history_t history;
  const cg::feedback_time_point_t start {};
  std::array<cg::packet_metric_t, 8> storage {};

  history.record(50, 3, 1000, start);

  const auto payload = build_report(1, 50, 0, {0, -1, -1});
  const auto report = cg::parse_packet_feedback(payload, storage);
  ASSERT_TRUE(report.has_value());

  const auto estimate = cg::analyze_packet_feedback(*report, history);

  EXPECT_EQ(estimate.lost_packets, 2u);
  EXPECT_EQ(estimate.delivered_packets, 1u);
  // One delivered packet establishes no interval to measure across.
  EXPECT_FALSE(estimate.valid);
}
