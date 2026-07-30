/**
 * @file tests/unit/test_bandwidth_estimator.cpp
 * @brief Tests for loss estimation over existing GameStream feedback.
 */
#include "../tests_common.h"

#include <chrono>
#include <src/bandwidth_estimator.h>

using namespace std::literals;

namespace {

  namespace congestion = stream::congestion;

  congestion::estimator_time_point_t epoch() {
    return congestion::estimator_time_point_t {};
  }

  /** @brief Feed n frames with the given per-frame delivery counters. */
  void observe(
    congestion::bandwidth_estimator_t &estimator,
    int frames,
    std::uint16_t data,
    std::uint16_t repair,
    std::uint16_t received_data,
    std::uint16_t received_repair,
    congestion::estimator_time_point_t now
  ) {
    for (int i = 0; i < frames; ++i) {
      estimator.observe_frame_feedback(
        data,
        repair,
        received_data,
        received_repair,
        now
      );
    }
  }

}  // namespace

TEST(FrameDeliveryTest, ClassifiesByReedSolomonRecoverability) {
  using congestion::classify_frame_delivery;
  using congestion::frame_delivery_e;

  // All data shards arrived.
  EXPECT_EQ(
    classify_frame_delivery(10, 2, 10, 0),
    frame_delivery_e::clean
  );
  // Two data shards lost, two repair shards arrived: rebuildable.
  EXPECT_EQ(
    classify_frame_delivery(10, 2, 8, 2),
    frame_delivery_e::recovered
  );
  // Three data shards lost, only two repair shards: not rebuildable.
  EXPECT_EQ(
    classify_frame_delivery(10, 2, 7, 2),
    frame_delivery_e::lost
  );
  // Repair shards substitute for any data shard, not a specific one.
  EXPECT_EQ(
    classify_frame_delivery(10, 5, 5, 5),
    frame_delivery_e::recovered
  );
  // Counters come off the wire: a peer over-reporting must not become
  // "recovered" for a frame that clearly was not.
  EXPECT_EQ(
    classify_frame_delivery(10, 0, 200, 200),
    frame_delivery_e::clean
  );
  EXPECT_EQ(classify_frame_delivery(0, 0, 0, 0), frame_delivery_e::clean);
}

TEST(BandwidthEstimatorTest, StaysInvalidBelowMinimumSamples) {
  congestion::bandwidth_estimator_t estimator;
  observe(estimator, 3, 10, 2, 7, 2, epoch());

  EXPECT_FALSE(estimator.estimate(epoch()).valid);
}

// The distinction the whole design rests on: heavy loss that FEC repaired is
// invisible to the user and must not read as degradation.
TEST(BandwidthEstimatorTest, RecoveredLossDoesNotCountAsUnrecovered) {
  congestion::bandwidth_estimator_t estimator;
  observe(estimator, 20, 10, 4, 7, 4, epoch());

  const auto estimate = estimator.estimate(epoch());
  ASSERT_TRUE(estimate.valid);
  // Three of fourteen shards missing per frame is real packet loss...
  EXPECT_GT(estimate.loss_ratio, 0.2);
  // ...but every frame was rebuildable, so nothing was user-visible.
  EXPECT_DOUBLE_EQ(estimate.unrecovered_loss_ratio, 0.0);
  EXPECT_EQ(estimate.unrecovered_frames, 0u);
}

TEST(BandwidthEstimatorTest, ReportsUnrecoverableFrames) {
  congestion::bandwidth_estimator_t estimator;
  observe(estimator, 10, 10, 2, 5, 2, epoch());

  const auto estimate = estimator.estimate(epoch());
  ASSERT_TRUE(estimate.valid);
  EXPECT_DOUBLE_EQ(estimate.unrecovered_loss_ratio, 1.0);
  EXPECT_DOUBLE_EQ(estimate.clean_frame_ratio, 0.0);
}

TEST(BandwidthEstimatorTest, CleanStreamReportsNoLoss) {
  congestion::bandwidth_estimator_t estimator;
  observe(estimator, 30, 10, 2, 10, 2, epoch());

  const auto estimate = estimator.estimate(epoch());
  ASSERT_TRUE(estimate.valid);
  EXPECT_DOUBLE_EQ(estimate.loss_ratio, 0.0);
  EXPECT_DOUBLE_EQ(estimate.unrecovered_loss_ratio, 0.0);
  EXPECT_DOUBLE_EQ(estimate.clean_frame_ratio, 1.0);
}

// Without a tumbling window, recovery after a bad patch would stay hidden
// behind the old samples.
TEST(BandwidthEstimatorTest, WindowExpiryForgetsOldConditions) {
  congestion::bandwidth_estimator_t estimator;
  estimator.reset(epoch());
  observe(estimator, 10, 10, 2, 5, 2, epoch());
  ASSERT_DOUBLE_EQ(
    estimator.estimate(epoch()).unrecovered_loss_ratio,
    1.0
  );

  const auto later =
    epoch() + congestion::bandwidth_estimator_t::sample_window + 1ms;
  // The window rolls, so the estimate is invalid again until it refills.
  EXPECT_FALSE(estimator.estimate(later).valid);

  observe(estimator, 10, 10, 2, 10, 2, later);
  const auto recovered = estimator.estimate(later);
  ASSERT_TRUE(recovered.valid);
  EXPECT_DOUBLE_EQ(recovered.unrecovered_loss_ratio, 0.0);
}

// A legacy aggregate report has no per-frame structure, so it can raise raw
// loss but must never imply the client failed to rebuild anything.
TEST(BandwidthEstimatorTest, LegacyLossRaisesRawRatioOnly) {
  congestion::bandwidth_estimator_t estimator;
  observe(estimator, 10, 10, 0, 10, 0, epoch());
  estimator.observe_legacy_loss(50, 100, epoch());

  const auto estimate = estimator.estimate(epoch());
  ASSERT_TRUE(estimate.valid);
  EXPECT_GT(estimate.loss_ratio, 0.0);
  EXPECT_DOUBLE_EQ(estimate.unrecovered_loss_ratio, 0.0);
}

TEST(BandwidthEstimatorTest, LegacyLossIgnoresDegenerateIntervals) {
  congestion::bandwidth_estimator_t estimator;
  observe(estimator, 10, 10, 0, 10, 0, epoch());
  // No packets sent means no ratio can be derived; must not divide or inflate.
  estimator.observe_legacy_loss(500, 0, epoch());

  const auto estimate = estimator.estimate(epoch());
  ASSERT_TRUE(estimate.valid);
  EXPECT_DOUBLE_EQ(estimate.loss_ratio, 0.0);
  // A peer claiming more loss than was sent cannot exceed 100%.
  estimator.observe_legacy_loss(1000, 10, epoch());
  EXPECT_LE(estimator.estimate(epoch()).loss_ratio, 1.0);
}
