/**
 * @file tests/unit/test_shard_dispatch.cpp
 * @brief Tests for the per-block shard batching/pacing loop.
 */
#include "../tests_common.h"

#include <chrono>
#include <src/frame_queue_policy.h>
#include <src/shard_dispatch.h>
#include <vector>

using namespace std::literals;

namespace {

  namespace dispatch = stream::dispatch;
  namespace pacing = stream::pacing;

  /**
   * @brief Pacer that reports a scripted deadline expiry after N batches.
   */
  class scripted_pacer_t final: public pacing::IPacketPacer {
  public:
    void begin_frame(const pacing::packet_pacing_config_t &) override {
    }

    [[nodiscard]] std::size_t maximum_batch_packets(
      std::size_t platform_limit
    ) const noexcept override {
      return platform_limit;
    }

    pacing::packet_pacing_wait_t wait_before_batch() override {
      ++wait_calls;
      if (expire_after_waits && wait_calls > *expire_after_waits) {
        return {.deadline_expired = true};
      }
      return {.waited = wait_duration};
    }

    void record_batch(std::size_t packet_count) noexcept override {
      recorded.push_back(packet_count);
    }

    void finish_block() noexcept override {
      ++finish_calls;
    }

    [[nodiscard]] pacing::pacer_time_point_t
      next_frame_start() const noexcept override {
      return {};
    }

    std::optional<int> expire_after_waits;
    pacing::pacer_duration_t wait_duration {};
    std::vector<std::size_t> recorded;
    int wait_calls = 0;
    int finish_calls = 0;
  };

  struct capture_t {
    std::vector<std::size_t> prepared;
    std::vector<std::pair<std::size_t, std::size_t>> batches;
    std::vector<std::size_t> reported_data_counts;
  };

  dispatch::shard_dispatch_callbacks_t make_callbacks(
    capture_t &capture,
    std::function<std::size_t(std::size_t, std::size_t)> send = {}
  ) {
    return {
      .prepare_shard =
        [&capture](std::size_t index) {
          capture.prepared.push_back(index);
        },
      .send_batch =
        [&capture, send](std::size_t offset, std::size_t count) -> std::size_t {
        capture.batches.emplace_back(offset, count);
        return send ? send(offset, count) : count;
      },
      .on_batch_sent =
        [&capture](
          std::size_t,
          std::size_t,
          std::size_t data_count,
          dispatch::dispatch_time_point_t
        ) {
          capture.reported_data_counts.push_back(data_count);
        },
    };
  }

}  // namespace

TEST(ShardDispatchTest, BatchesEveryShardWhenNothingExpires) {
  scripted_pacer_t pacer;
  capture_t capture;

  const auto result = dispatch::dispatch_shards(
    {
      .shard_count = 10,
      .data_shard_count = 8,
      .batch_size = 4,
      .wire_bytes_per_shard = 100,
    },
    pacer,
    make_callbacks(capture)
  );

  EXPECT_EQ(result.shards_sent, 10u);
  EXPECT_EQ(result.data_shards_sent, 8u);
  EXPECT_EQ(result.repair_shards_sent, 2u);
  EXPECT_EQ(result.wire_bytes_sent, 1000u);
  EXPECT_FALSE(result.deadline_expired);
  EXPECT_EQ(capture.prepared.size(), 10u);
  // 4 + 4 + 2, the trailing partial batch flushes at the end of the block.
  ASSERT_EQ(capture.batches.size(), 3u);
  EXPECT_EQ(capture.batches[0], std::make_pair(std::size_t {0}, std::size_t {4}));
  EXPECT_EQ(capture.batches[1], std::make_pair(std::size_t {4}, std::size_t {4}));
  EXPECT_EQ(capture.batches[2], std::make_pair(std::size_t {8}, std::size_t {2}));
  EXPECT_EQ(pacer.recorded, (std::vector<std::size_t> {4, 4, 2}));
}

// The regression this whole extraction exists for: a frame aborted partway
// must report only what it sent, while the caller still advances its sequence
// space by every stamped shard.
TEST(ShardDispatchTest, PartialSendReportsSentButConsumesEverySequenceNumber) {
  scripted_pacer_t pacer;
  pacer.expire_after_waits = 2;  // third batch is refused
  capture_t capture;

  const auto result = dispatch::dispatch_shards(
    {
      .shard_count = 12,
      .data_shard_count = 10,
      .batch_size = 4,
      .wire_bytes_per_shard = 100,
    },
    pacer,
    make_callbacks(capture)
  );

  ASSERT_TRUE(result.deadline_expired);
  EXPECT_EQ(result.shards_sent, 8u);
  EXPECT_EQ(result.wire_bytes_sent, 800u);
  EXPECT_EQ(capture.batches.size(), 2u);

  // What actually left the host is 8, but 12 sequence numbers were stamped.
  EXPECT_EQ(
    stream::queueing::sequence_numbers_consumed(12, result.shards_sent),
    12u
  );
}

TEST(ShardDispatchTest, StopsImmediatelyWhenTheFirstBatchIsRefused) {
  scripted_pacer_t pacer;
  pacer.expire_after_waits = 0;
  capture_t capture;

  const auto result = dispatch::dispatch_shards(
    {.shard_count = 6, .data_shard_count = 6, .batch_size = 2},
    pacer,
    make_callbacks(capture)
  );

  EXPECT_TRUE(result.deadline_expired);
  EXPECT_EQ(result.shards_sent, 0u);
  EXPECT_EQ(result.wire_bytes_sent, 0u);
  EXPECT_TRUE(capture.batches.empty());
  EXPECT_FALSE(result.first_sent_at.has_value());
  EXPECT_FALSE(result.last_sent_at.has_value());
}

// The unbatched fallback can stop midway when it notices the deadline, which
// the transport reports as a short count.
TEST(ShardDispatchTest, TreatsShortBatchAsDeadlineAbort) {
  scripted_pacer_t pacer;
  capture_t capture;

  const auto result = dispatch::dispatch_shards(
    {
      .shard_count = 8,
      .data_shard_count = 8,
      .batch_size = 4,
      .wire_bytes_per_shard = 10,
    },
    pacer,
    make_callbacks(capture, [](std::size_t offset, std::size_t count) {
      return offset == 0 ? count : count - 3;  // second batch sends 1 of 4
    })
  );

  EXPECT_TRUE(result.deadline_expired);
  EXPECT_EQ(result.shards_sent, 5u);
  EXPECT_EQ(result.wire_bytes_sent, 50u);
  EXPECT_EQ(pacer.recorded, (std::vector<std::size_t> {4, 1}));
}

TEST(ShardDispatchTest, SplitsDataAndRepairShardsAcrossBatchBoundaries) {
  scripted_pacer_t pacer;
  capture_t capture;

  // 5 data + 5 repair with a batch straddling the boundary at index 5.
  const auto result = dispatch::dispatch_shards(
    {.shard_count = 10, .data_shard_count = 5, .batch_size = 4},
    pacer,
    make_callbacks(capture)
  );

  EXPECT_EQ(result.data_shards_sent, 5u);
  EXPECT_EQ(result.repair_shards_sent, 5u);
  // Batches are [0,4) [4,8) [8,10): 4 data, then 1 data, then 0 data.
  EXPECT_EQ(
    capture.reported_data_counts,
    (std::vector<std::size_t> {4, 1, 0})
  );
}

TEST(ShardDispatchTest, HandlesEmptyBlockAndSingleShardBlocks) {
  scripted_pacer_t pacer;
  capture_t empty_capture;

  const auto empty = dispatch::dispatch_shards(
    {.shard_count = 0},
    pacer,
    make_callbacks(empty_capture)
  );
  EXPECT_EQ(empty.shards_sent, 0u);
  EXPECT_FALSE(empty.deadline_expired);
  EXPECT_EQ(pacer.wait_calls, 0);

  capture_t single_capture;
  const auto single = dispatch::dispatch_shards(
    {.shard_count = 1, .data_shard_count = 1, .batch_size = 8},
    pacer,
    make_callbacks(single_capture)
  );
  EXPECT_EQ(single.shards_sent, 1u);
  EXPECT_EQ(single_capture.batches.size(), 1u);
}

// A zero batch size would otherwise flush on every shard or divide badly.
TEST(ShardDispatchTest, ClampsDegenerateBatchSize) {
  scripted_pacer_t pacer;
  capture_t capture;

  const auto result = dispatch::dispatch_shards(
    {.shard_count = 3, .data_shard_count = 3, .batch_size = 0},
    pacer,
    make_callbacks(capture)
  );

  EXPECT_EQ(result.shards_sent, 3u);
  EXPECT_EQ(capture.batches.size(), 3u);
}

TEST(ShardDispatchTest, AccumulatesPacerAndSendTime) {
  scripted_pacer_t pacer;
  pacer.wait_duration = 5ms;
  capture_t capture;

  const auto result = dispatch::dispatch_shards(
    {.shard_count = 8, .data_shard_count = 8, .batch_size = 4},
    pacer,
    make_callbacks(capture)
  );

  EXPECT_EQ(result.pacer_time, 10ms);  // two batches
  EXPECT_GE(result.send_time, dispatch::dispatch_duration_t::zero());
  EXPECT_TRUE(result.first_sent_at.has_value());
  EXPECT_TRUE(result.last_sent_at.has_value());
}
