/**
 * @file tests/unit/test_video_loss_monitor.cpp
 * @brief The video a client reports losing, from its SS_FRAME_FEC_STATUS reports.
 */
#include "../tests_common.h"

#include <array>
#include <src/video_loss_monitor.h>
#include <string>

namespace {
  using stream::video_loss_monitor_t;

  /// An SS_FRAME_FEC_STATUS payload as moonlight-common-c sends it: packed, big-endian.
  std::string fec_status(std::uint32_t frame, std::uint16_t total_data, std::uint16_t total_parity, std::uint16_t received_data, std::uint16_t received_parity) {
    std::string out;
    const auto put16 = [&out](std::uint16_t v) {
      out.push_back(static_cast<char>(v >> 8));
      out.push_back(static_cast<char>(v & 0xff));
    };
    out.push_back(static_cast<char>(frame >> 24));
    out.push_back(static_cast<char>(frame >> 16 & 0xff));
    out.push_back(static_cast<char>(frame >> 8 & 0xff));
    out.push_back(static_cast<char>(frame & 0xff));
    put16(0x1234);  // highest received sequence number
    put16(0x1200);  // next contiguous sequence number
    put16(7);  // missing before the highest received
    put16(total_data);
    put16(total_parity);
    put16(received_data);
    put16(received_parity);
    out.push_back(20);  // FEC percentage
    out.push_back(0);  // block index
    out.push_back(1);  // block count
    return out;
  }
}  // namespace

TEST(VideoLossMonitor, ParsesTheWireFormat) {
  const auto payload = fec_status(0x01020304, 300, 60, 290, 55);
  ASSERT_EQ(payload.size(), video_loss_monitor_t::report_size);

  const auto report = video_loss_monitor_t::parse(payload);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->frame_index, 0x01020304u);
  EXPECT_EQ(report->total_data, 300);
  EXPECT_EQ(report->total_parity, 60);
  EXPECT_EQ(report->received_data, 290);
  EXPECT_EQ(report->received_parity, 55);
  EXPECT_EQ(report->missing_packets(), 15u);
  EXPECT_FALSE(report->lost()) << "345 packets arrived; 300 rebuild the block";
}

TEST(VideoLossMonitor, RejectsShortOrEmptyReports) {
  const auto payload = fec_status(1, 10, 2, 9, 2);
  EXPECT_FALSE(video_loss_monitor_t::parse(payload.substr(0, payload.size() - 1)));
  EXPECT_FALSE(video_loss_monitor_t::parse(fec_status(1, 0, 0, 0, 0))) << "a block with no data packets is not a report";
}

TEST(VideoLossMonitor, RepairedBlocksAreNotDroppedFrames) {
  video_loss_monitor_t loss;
  loss.on_report(*video_loss_monitor_t::parse(fec_status(10, 100, 20, 95, 20)));
  loss.on_report(*video_loss_monitor_t::parse(fec_status(11, 100, 20, 100, 18)));

  EXPECT_EQ(loss.repaired_blocks(), 2u);
  EXPECT_EQ(loss.dropped_frames(), 0u);
  EXPECT_EQ(loss.missing_packets(), 7u);
}

TEST(VideoLossMonitor, AFrameWithSeveralLostBlocksIsOneDroppedFrame) {
  video_loss_monitor_t loss;
  // Frame 20 lost two of its blocks; frame 21 lost one.
  loss.on_report(*video_loss_monitor_t::parse(fec_status(20, 100, 20, 70, 10)));
  loss.on_report(*video_loss_monitor_t::parse(fec_status(20, 100, 20, 80, 5)));
  loss.on_report(*video_loss_monitor_t::parse(fec_status(21, 50, 10, 10, 0)));

  EXPECT_EQ(loss.dropped_frames(), 2u);
  EXPECT_EQ(loss.repaired_blocks(), 0u);
  EXPECT_EQ(loss.missing_packets(), 40u + 35u + 50u);
}

TEST(VideoLossMonitor, CountsPacketsSent) {
  video_loss_monitor_t loss;
  loss.on_sent(120);
  loss.on_sent(60);
  EXPECT_EQ(loss.sent_packets(), 180u);
  EXPECT_EQ(loss.missing_packets(), 0u);
}
