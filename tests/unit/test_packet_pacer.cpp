/**
 * @file tests/unit/test_packet_pacer.cpp
 * @brief Deterministic tests for the injectable Hermes packet pacer.
 */

#include "../tests_common.h"

#include <chrono>
#include <src/packet_pacer.h>
#include <vector>

using namespace std::literals;

namespace {

  class fake_pacer_timer_t final:
      public stream::pacing::IPacerTimer {
  public:
    [[nodiscard]] stream::pacing::pacer_time_point_t
      now() const noexcept override {
      ++now_calls;
      return current;
    }

    void sleep_for(
      const std::chrono::nanoseconds &duration
    ) override {
      waits.push_back(duration);
      current += duration + oversleep;
    }

    void advance(stream::pacing::pacer_duration_t duration) {
      current += duration;
    }

    stream::pacing::pacer_time_point_t current {};
    std::chrono::nanoseconds oversleep {};
    std::vector<std::chrono::nanoseconds> waits;
    mutable int now_calls = 0;
  };

  class fake_packet_pacer_t final:
      public stream::pacing::IPacketPacer {
  public:
    void begin_frame(
      const stream::pacing::packet_pacing_config_t &config
    ) override {
      ++begin_calls;
      last_config = config;
    }

    [[nodiscard]] std::size_t maximum_batch_packets(
      std::size_t platform_limit
    ) const noexcept override {
      return std::min(platform_limit, batch_limit);
    }

    stream::pacing::packet_pacing_wait_t
      wait_before_batch() override {
      ++wait_calls;
      return {
        .waited = wait_duration,
        .deadline_expired = deadline_expired,
      };
    }

    void record_batch(std::size_t packet_count) noexcept override {
      recorded_packets += packet_count;
    }

    void finish_block() noexcept override {
      ++finished_blocks;
    }

    [[nodiscard]] stream::pacing::pacer_time_point_t
      next_frame_start() const noexcept override {
      return next_start;
    }

    stream::pacing::pacer_duration_t wait_duration {};
    bool deadline_expired = false;
    stream::pacing::pacer_time_point_t next_start {};
    stream::pacing::packet_pacing_config_t last_config;
    std::size_t batch_limit = 4;
    std::size_t recorded_packets = 0;
    int begin_calls = 0;
    int wait_calls = 0;
    int finished_blocks = 0;
  };

  stream::pacing::packet_pacing_config_t legacy_config(
    std::size_t packet_size_bytes = 10000
  ) {
    return {
      .packet_size_bytes = packet_size_bytes,
      .bitrate_bps = 800'000'000,
      .max_burst_duration = 1ms,
    };
  }

}  // namespace

TEST(PacketPacerTest, InterfaceSupportsFakeImplementation) {
  fake_packet_pacer_t fake;
  fake.wait_duration = 250us;
  fake.deadline_expired = true;
  fake.next_start =
    stream::pacing::pacer_time_point_t {} + 4ms;
  const auto deadline =
    stream::pacing::pacer_time_point_t {} + 3ms;
  stream::pacing::IPacketPacer &pacer = fake;

  pacer.begin_frame({
    .packet_size_bytes = 1200,
    .bitrate_bps = 25'000'000,
    .max_burst_duration = 1ms,
    .packet_deadline = deadline,
  });
  EXPECT_EQ(pacer.maximum_batch_packets(64), 4);
  const auto wait = pacer.wait_before_batch();
  EXPECT_EQ(wait.waited, 250us);
  EXPECT_TRUE(wait.deadline_expired);
  pacer.record_batch(7);
  pacer.finish_block();

  EXPECT_EQ(pacer.next_frame_start(), fake.next_start);
  EXPECT_EQ(fake.begin_calls, 1);
  EXPECT_EQ(fake.wait_calls, 1);
  EXPECT_EQ(fake.last_config.packet_size_bytes, 1200);
  EXPECT_EQ(fake.last_config.bitrate_bps, 25'000'000);
  EXPECT_EQ(fake.last_config.packet_deadline, deadline);
  EXPECT_EQ(fake.recorded_packets, 7);
  EXPECT_EQ(fake.finished_blocks, 1);
}

TEST(PacketPacerTest, LegacyPolicyPreservesOneMillisecondGroupThreshold) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  // At 10,000 bytes, the legacy 80%-of-1-Gbps formula yields 10 packets/ms.
  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);

  pacer.record_batch(5);
  const auto calls_before_partial_group = timer.now_calls;
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  EXPECT_EQ(timer.now_calls, calls_before_partial_group);

  pacer.record_batch(5);
  EXPECT_EQ(pacer.wait_before_batch().waited, 1ms);
  ASSERT_EQ(timer.waits.size(), 1);
  EXPECT_EQ(timer.waits.front(), 1ms);
  EXPECT_EQ(timer.current.time_since_epoch(), 1ms);
}

TEST(PacketPacerTest, LegacyPolicyCarriesLastFrameGroupIntoNextFrame) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(3);
  pacer.finish_block();

  EXPECT_EQ(
    pacer.next_frame_start().time_since_epoch(),
    300us
  );

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 300us);
  ASSERT_EQ(timer.waits.size(), 1);
  EXPECT_EQ(timer.waits.front(), 300us);
}

TEST(PacketPacerTest, LegacyPolicyStartsFromNowAfterAnIdleGap) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(3);
  pacer.finish_block();
  timer.advance(2ms);

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(3);
  pacer.finish_block();

  EXPECT_EQ(
    pacer.next_frame_start().time_since_epoch(),
    2300us
  );
  EXPECT_TRUE(timer.waits.empty());
}

TEST(PacketPacerTest, LegacyPolicyCountsPacketsAcrossFecBlocks) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(3);
  pacer.finish_block();
  EXPECT_EQ(
    pacer.next_frame_start().time_since_epoch(),
    300us
  );

  // A new FEC block does not begin a new pacing frame.
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(2);
  pacer.finish_block();
  EXPECT_EQ(
    pacer.next_frame_start().time_since_epoch(),
    500us
  );
}

TEST(PacketPacerTest, LegacyPolicyReportsActualOversleptDuration) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(3);
  pacer.finish_block();

  pacer.begin_frame(legacy_config());
  timer.oversleep = 125us;
  EXPECT_EQ(pacer.wait_before_batch().waited, 425us);
  ASSERT_EQ(timer.waits.size(), 1);
  EXPECT_EQ(timer.waits.front(), 300us);
}

TEST(PacketPacerTest, LegacyPolicySkipsWaitWhenAlreadyPastDeadline) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config());
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(10);
  timer.advance(2ms);

  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  EXPECT_TRUE(timer.waits.empty());
}

TEST(PacketPacerTest, LegacyPolicyDoesNotWaitPastPacketDeadline) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config());
  pacer.record_batch(3);
  pacer.finish_block();

  auto config = legacy_config();
  config.packet_deadline =
    stream::pacing::pacer_time_point_t {} + 200us;
  pacer.begin_frame(config);
  const auto expired = pacer.wait_before_batch();

  EXPECT_TRUE(expired.deadline_expired);
  EXPECT_EQ(expired.waited, 0ns);
  EXPECT_TRUE(timer.waits.empty());
}

TEST(PacketPacerTest, RateLimitedPolicyCapsBatchToOneMillisecond) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1000,
    .bitrate_bps = 32'000'000,
    .max_burst_duration = 1ms,
  });

  EXPECT_EQ(pacer.maximum_batch_packets(64), 4);
  EXPECT_EQ(pacer.maximum_batch_packets(2), 2);
}

TEST(PacketPacerTest, RateLimitedPolicySpacesConsecutiveBatches) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1000,
    .bitrate_bps = 8'000'000,
    .max_burst_duration = 1ms,
  });
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(1);
  EXPECT_EQ(pacer.wait_before_batch().waited, 1ms);
  pacer.record_batch(1);
  pacer.finish_block();

  EXPECT_EQ(
    pacer.next_frame_start().time_since_epoch(),
    2ms
  );
}

TEST(PacketPacerTest, RateLimitedPolicyDoesNotCatchUpAfterOversleep) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1000,
    .bitrate_bps = 8'000'000,
    .max_burst_duration = 1ms,
  });
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(1);

  timer.oversleep = 500us;
  EXPECT_EQ(pacer.wait_before_batch().waited, 1500us);
  pacer.record_batch(1);
  EXPECT_EQ(pacer.wait_before_batch().waited, 1500us);

  ASSERT_EQ(timer.waits.size(), 2);
  EXPECT_EQ(timer.waits[0], 1ms);
  EXPECT_EQ(timer.waits[1], 1ms);
}

TEST(PacketPacerTest, RateLimitedPolicySkipsWaitPastPacketDeadline) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1000,
    .bitrate_bps = 8'000'000,
    .max_burst_duration = 1ms,
    .packet_deadline =
      stream::pacing::pacer_time_point_t {} + 500us,
  });
  EXPECT_FALSE(pacer.wait_before_batch().deadline_expired);
  pacer.record_batch(1);

  const auto expired = pacer.wait_before_batch();
  EXPECT_TRUE(expired.deadline_expired);
  EXPECT_EQ(expired.waited, 0ns);
  EXPECT_TRUE(timer.waits.empty());
}

TEST(PacketPacerTest, RateLimitedPolicyDetectsDeadlineOversleep) {
  fake_pacer_timer_t timer;
  timer.oversleep = 500us;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1000,
    .bitrate_bps = 8'000'000,
    .max_burst_duration = 1ms,
    .packet_deadline =
      stream::pacing::pacer_time_point_t {} + 1200us,
  });
  EXPECT_FALSE(pacer.wait_before_batch().deadline_expired);
  pacer.record_batch(1);

  const auto expired = pacer.wait_before_batch();
  EXPECT_TRUE(expired.deadline_expired);
  EXPECT_EQ(expired.waited, 1500us);
  ASSERT_EQ(timer.waits.size(), 1);
  EXPECT_EQ(timer.waits.front(), 1ms);
}

TEST(PacketPacerTest, RateLimitedPolicyAllowsExactPacketDeadline) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1000,
    .bitrate_bps = 8'000'000,
    .max_burst_duration = 1ms,
    .packet_deadline =
      stream::pacing::pacer_time_point_t {} + 1ms,
  });
  EXPECT_FALSE(pacer.wait_before_batch().deadline_expired);
  pacer.record_batch(1);

  const auto at_deadline = pacer.wait_before_batch();
  EXPECT_FALSE(at_deadline.deadline_expired);
  EXPECT_EQ(at_deadline.waited, 1ms);
}

TEST(PacketPacerTest, RateLimitedPolicySpacesAudioRepairBurst) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};
  constexpr std::uint64_t three_packets_per_five_ms =
    1200ULL * 8 * 3 * 1000 / 5;

  pacer.begin_frame({
    .packet_size_bytes = 1200,
    .bitrate_bps = three_packets_per_five_ms,
    .max_burst_duration = 1ms,
  });
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(1);
  EXPECT_EQ(pacer.wait_before_batch().waited, 1666667ns);
  pacer.record_batch(1);
  EXPECT_EQ(pacer.wait_before_batch().waited, 1666667ns);
  pacer.record_batch(1);
  pacer.finish_block();

  EXPECT_EQ(
    pacer.next_frame_start().time_since_epoch(),
    5000001ns
  );
}

TEST(PacketPacerTest, RateLimitedRegistryReusesSessionPacer) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_pacer_registry_t registry {timer};
  const auto session =
    reinterpret_cast<const void *>(std::uintptr_t {1});

  auto *first = &registry.for_session(session);
  auto *second = &registry.for_session(session);

  EXPECT_EQ(first, second);
  EXPECT_EQ(registry.active_slots(), 1);
}

TEST(PacketPacerTest, RateLimitedRegistryBoundsSessionChurn) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_pacer_registry_t registry {timer};

  for (
    std::size_t index = 0;
    index < stream::pacing::rate_limited_pacer_registry_t::
                max_sessions +
              5;
    ++index) {
    static_cast<void>(registry.for_session(
      reinterpret_cast<const void *>(
        static_cast<std::uintptr_t>(index + 1)
      )
    ));
  }

  EXPECT_EQ(
    registry.active_slots(),
    stream::pacing::rate_limited_pacer_registry_t::max_sessions
  );
}

// A packet larger than 100000 bytes truncates the legacy 80%-of-1-Gbps integer
// formula to zero packets per quantum, which is used as a divisor.
TEST(PacketPacerTest, LegacyPolicySurvivesPacketLargerThanOneQuantum) {
  fake_pacer_timer_t timer;
  stream::pacing::legacy_packet_pacer_t pacer {timer};

  pacer.begin_frame(legacy_config(200'000));
  EXPECT_EQ(pacer.wait_before_batch().waited, 0ns);
  pacer.record_batch(1);
  // Would divide by zero without the quantum clamp.
  static_cast<void>(pacer.wait_before_batch());
  pacer.finish_block();
  EXPECT_GE(
    pacer.next_frame_start().time_since_epoch(),
    stream::pacing::pacer_duration_t::zero()
  );
}

// maximum_batch_packets() is public and const, so it must not rely on
// begin_frame() having clamped the burst duration first.
TEST(PacketPacerTest, RateLimitedPolicyToleratesZeroBurstDuration) {
  fake_pacer_timer_t timer;
  stream::pacing::rate_limited_packet_pacer_t pacer {timer};

  pacer.begin_frame({
    .packet_size_bytes = 1200,
    .bitrate_bps = 25'000'000,
    .max_burst_duration = 0ns,
  });
  EXPECT_GE(pacer.maximum_batch_packets(64), 1u);
}
