/**
 * @file tests/unit/test_adaptive_fec.cpp
 * @brief Sizing parity from what the client reported losing.
 */
#include "../tests_common.h"

#include <src/adaptive_fec.h>

namespace {
  using stream::adaptive_fec_t;

  /// One frame's worth of the counters a session would hand the controller.
  struct counters_t {
    std::uint64_t sent {0};
    std::uint64_t missing {0};
    std::uint64_t reported {0};
    std::int64_t now_ms {0};
  };

  /**
   * Feed one second of traffic at 60 fps and return the percentage the host would use.
   *
   * Reports only happen when something was lost: a client with a clean link has
   * nothing to report, so @p loss also decides whether any report is sent.
   */
  int run_second(adaptive_fec_t &fec, counters_t &counters, double loss, int configured = 20, int ceiling = 50) {
    constexpr int shards_per_frame = 40;
    const auto start_ms = counters.now_ms;
    int last = configured;
    for (auto frame = 0; frame < 60; ++frame) {
      counters.sent += shards_per_frame;
      counters.missing += static_cast<std::uint64_t>(loss * shards_per_frame);
      if (loss > 0.0) {
        // A block that lost packets is what the client reports on.
        counters.reported += 1;
      }
      // Whole milliseconds, but still a full second after 60 frames.
      counters.now_ms = start_ms + (frame + 1) * 1000 / 60;
      last = fec.percentage(counters.sent, counters.missing, counters.reported, counters.now_ms, configured, ceiling);
    }
    return last;
  }

  /// Feed @p seconds of traffic, returning the percentage after the last one.
  ///
  /// The first second of a session only starts the baseline, so a caller that wants
  /// an estimate has to feed at least two.
  int run_seconds(adaptive_fec_t &fec, counters_t &counters, double loss, int seconds,
                  int configured = 20, int ceiling = 50) {
    int last = configured;
    for (auto i = 0; i < seconds; ++i) {
      last = run_second(fec, counters, loss, configured, ceiling);
    }
    return last;
  }

  /// Call the controller once with the counters as they stand.
  int sample(adaptive_fec_t &fec, counters_t &counters, int configured = 20, int ceiling = 50) {
    return fec.percentage(counters.sent, counters.missing, counters.reported, counters.now_ms, configured, ceiling);
  }

  /**
   * Like run_second, but the second is cut to @p span_ms, the way jitter cuts a rollover.
   */
  int run_second_of_span(adaptive_fec_t &fec, counters_t &counters, double loss, std::int64_t span_ms,
                         int configured = 20, int ceiling = 50) {
    constexpr int shards_per_frame = 40;
    const auto start_ms = counters.now_ms;
    int last = configured;
    for (auto frame = 0; frame < 60; ++frame) {
      counters.sent += shards_per_frame;
      counters.missing += static_cast<std::uint64_t>(loss * shards_per_frame);
      if (loss > 0.0) {
        counters.reported += 1;
      }
      counters.now_ms = start_ms + (frame + 1) * span_ms / 60;
      last = fec.percentage(counters.sent, counters.missing, counters.reported, counters.now_ms, configured, ceiling);
    }
    return last;
  }
}  // namespace

TEST(AdaptiveFec, TrafficWithoutReportsKeepsTheOperatorsValue) {
  // sent_packets() advances for every block the host transmits, so a client that never
  // sends SS_FRAME_FEC_STATUS would look like a perfect link if traffic counted as
  // evidence. It must not: without a report there is nothing to size parity from.
  adaptive_fec_t fec;
  counters_t counters;
  for (auto second = 0; second < 10; ++second) {
    counters.sent += 40 * 60;
    counters.now_ms += 1000;
    EXPECT_EQ(sample(fec, counters), 20) << "second " << second;
  }
}

TEST(AdaptiveFec, AReportedLossRaisesParity) {
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_seconds(fec, counters, 0.05, 2), 40);
}

TEST(AdaptiveFec, ACleanSecondAfterReportsRelaxesOnlyAfterTheCooldown) {
  // The client has proven it reports, so later silence is evidence of a clean link —
  // but the operator's value is held for a full cooldown before parity is given back.
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_seconds(fec, counters, 0.20, 2), 50);
  auto seconds = 0;
  auto value = 50;
  while (value != adaptive_fec_t::min_percentage && seconds < 12) {
    value = run_second(fec, counters, 0.0);
    ++seconds;
  }
  EXPECT_EQ(value, adaptive_fec_t::min_percentage);
  EXPECT_GE(seconds, adaptive_fec_t::lower_cooldown_ms / 1000);
}

TEST(AdaptiveFec, ARaiseRestartsTheCooldown) {
  // A burst in the middle of a quiet stretch must not hand parity back immediately.
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_seconds(fec, counters, 0.20, 2), 50);
  // Quiet, but nowhere near five seconds: parity stays.
  EXPECT_EQ(run_seconds(fec, counters, 0.0, 2), 50);
  // A fresh burst pushes the hold back out.
  EXPECT_EQ(run_seconds(fec, counters, 0.20, 1), 50);
  EXPECT_EQ(run_seconds(fec, counters, 0.0, 1), 50);
}

TEST(AdaptiveFec, HoldsTheHigherValueThroughTheWindowThenRelaxes) {
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_seconds(fec, counters, 0.20, 2), 50);
  // The window still remembers the bad second, so nothing drops immediately.
  EXPECT_EQ(run_second(fec, counters, 0.0), 50);
  for (auto second = 0; second < adaptive_fec_t::window_seconds; ++second) {
    run_second(fec, counters, 0.0);
  }
  // Once the bad second leaves the window, parity falls back to the minimum.
  EXPECT_EQ(run_second(fec, counters, 0.0), adaptive_fec_t::min_percentage);
}

TEST(AdaptiveFec, RespectsTheCeiling) {
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_seconds(fec, counters, 0.30, 2, 20, 12), 12);
}

TEST(AdaptiveFec, ASliverOfASecondIsNotAnEstimate) {
  // Crossing a clock-second boundary is not the same as sampling a second: a baseline
  // at 1990 ms and the next call at 2006 ms covers 16 ms, however many packets it had.
  adaptive_fec_t fec;
  counters_t counters;
  counters.reported = 1;
  counters.now_ms = 1990;
  EXPECT_EQ(sample(fec, counters), 20);
  counters.sent += 400;
  counters.now_ms = 2006;
  EXPECT_EQ(sample(fec, counters), 20) << "a 16 ms sliver must not be trusted";
}

TEST(AdaptiveFec, StaleBucketsLeaveTheWindow) {
  // A stall must not leave an old loss sample holding parity up: after resuming, a
  // sample from twenty seconds ago is outside the window even though its slot has
  // not been overwritten yet.
  adaptive_fec_t fec;
  counters_t counters;
  counters.reported = 1;
  counters.now_ms = 101000;
  EXPECT_EQ(run_seconds(fec, counters, 0.20, 2), 50);

  // Resume well past the window with a clean second.
  counters.now_ms = 125000;
  counters.sent += 40 * 60;
  EXPECT_EQ(sample(fec, counters), adaptive_fec_t::min_percentage);
}

TEST(AdaptiveFec, TheFirstCleanSampleDoesNotLowerWithALateClockOrigin) {
  // A monotonic clock that is already past the cooldown when the session starts must
  // not let the very first clean sample give parity away.
  adaptive_fec_t fec;
  counters_t counters;
  counters.now_ms = 10'000'000;
  EXPECT_EQ(run_seconds(fec, counters, 0.05, 2), 40);
  EXPECT_EQ(run_second(fec, counters, 0.0), 40) << "the cooldown starts at the first sample";
}

TEST(AdaptiveFec, ASecondCutShortByAClockBoundaryStillCounts) {
  // A rollover happens when a frame lands in the next second, so a bucket can cover
  // 998 ms rather than 1000. Discarding it would also start the next bucket short and
  // push the first estimate out by seconds.
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_second_of_span(fec, counters, 0.05, 998), 20) << "the first second only sets the baseline";
  EXPECT_EQ(run_second_of_span(fec, counters, 0.05, 998), 40) << "998 ms is a sample, not a sliver";
}

TEST(AdaptiveFec, AGapWithoutTrafficHoldsTheLastDecision) {
  // Silence ages every bucket out of the window. That is not a clean link: the last
  // decision stands until a trustworthy second says otherwise.
  adaptive_fec_t fec;
  counters_t counters;
  EXPECT_EQ(run_seconds(fec, counters, 0.20, 2), 50);
  counters.now_ms += 20'000;
  EXPECT_EQ(sample(fec, counters), 50) << "a gap must not hand parity back";
}

TEST(AdaptiveFec, PercentageGrowsWithLossAndStaysInBounds) {
  auto previous = 0;
  for (auto loss = 0.0; loss <= 0.6; loss += 0.005) {
    const auto percentage = adaptive_fec_t::percentage_for(loss, 50);
    EXPECT_GE(percentage, adaptive_fec_t::min_percentage);
    EXPECT_LE(percentage, adaptive_fec_t::max_percentage);
    EXPECT_GE(percentage, previous) << "loss " << loss;
    previous = percentage;
  }
}

TEST(AdaptiveFec, OnePercentLossNeedsAboutTwentyPercentParity) {
  // Ten data shards need two parity shards before a block is unlikely to be lost.
  EXPECT_EQ(adaptive_fec_t::percentage_for(0.01, 50), 20);
}

TEST(AdaptiveFec, UnrecoverableChanceIsTheUpperBinomialTail) {
  // With 10 data and 1 parity shard, losing 2 of 11 loses the block.
  const auto chance = adaptive_fec_t::unrecoverable_chance(10, 1, 0.01);
  EXPECT_GT(chance, 0.005);
  EXPECT_LT(chance, 0.006);
  EXPECT_EQ(adaptive_fec_t::unrecoverable_chance(10, 4, 0.0), 0.0);
}
