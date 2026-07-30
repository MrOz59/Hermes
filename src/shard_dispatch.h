/**
 * @file src/shard_dispatch.h
 * @brief Testable batching/pacing loop for one emitted FEC block.
 */
#pragma once

#include "packet_pacer.h"
#include "transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace stream::dispatch {

  using dispatch_clock_t = std::chrono::steady_clock;
  using dispatch_time_point_t = dispatch_clock_t::time_point;
  using dispatch_duration_t = dispatch_clock_t::duration;

  /**
   * @brief Per-block inputs for the shard dispatch loop.
   *
   * `shard_count` is the number of shards that were stamped with a sequence
   * number, which is what the caller must advance its sequence space by --
   * see stream::queueing::sequence_numbers_consumed().
   */
  struct shard_dispatch_request_t {
    std::size_t shard_count = 0;
    std::size_t data_shard_count = 0;
    std::size_t batch_size = 1;
    std::size_t wire_bytes_per_shard = 0;
    /// Absolute host-departure deadline. Empty disables expiry.
    std::optional<dispatch_time_point_t> packet_deadline;
  };

  /**
   * @brief What actually left the host for one block.
   *
   * `shards_sent` counts shards submitted to the transport. It is always less
   * than or equal to `shard_count`; it is never the value the caller should
   * advance sequence numbers by.
   */
  struct shard_dispatch_result_t {
    std::size_t shards_sent = 0;
    std::size_t data_shards_sent = 0;
    std::size_t repair_shards_sent = 0;
    std::size_t wire_bytes_sent = 0;
    dispatch_duration_t pacer_time {};
    dispatch_duration_t send_time {};
    bool deadline_expired = false;
    std::optional<dispatch_time_point_t> first_sent_at;
    std::optional<dispatch_time_point_t> last_sent_at;
  };

  /**
   * @brief Callbacks the loop needs from its caller.
   *
   * `prepare_shard` stamps and optionally encrypts one shard before it may be
   * batched. `send_batch` submits [offset, offset + count) and returns how many
   * were actually processed, which is below `count` when a fallback path hit
   * the deadline partway. `on_batch_sent` reports each accepted batch so the
   * caller can feed its congestion controller.
   */
  struct shard_dispatch_callbacks_t {
    std::function<void(std::size_t index)> prepare_shard;
    std::function<std::size_t(std::size_t offset, std::size_t count)> send_batch;
    std::function<void(
      std::size_t offset,
      std::size_t count,
      std::size_t data_count,
      dispatch_time_point_t sent_at
    )>
      on_batch_sent;
  };

  /**
   * @brief Batch, pace, and submit the shards of one FEC block.
   *
   * Shards are prepared in order and flushed whenever a full batch accumulates
   * or the block ends. The pacer is consulted before each batch, and the loop
   * stops early when the deadline expires, leaving the remaining shards unsent.
   *
   * The caller keeps ownership of the sequence space: this function reports
   * only what was sent and never advances anything itself.
   */
  [[nodiscard]] shard_dispatch_result_t dispatch_shards(
    const shard_dispatch_request_t &request,
    pacing::IPacketPacer &pacer,
    const shard_dispatch_callbacks_t &callbacks
  );

}  // namespace stream::dispatch
