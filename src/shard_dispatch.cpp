/**
 * @file src/shard_dispatch.cpp
 * @brief Batching/pacing loop for one emitted FEC block.
 */

#include "shard_dispatch.h"

#include <algorithm>

namespace stream::dispatch {

  shard_dispatch_result_t dispatch_shards(
    const shard_dispatch_request_t &request,
    pacing::IPacketPacer &pacer,
    const shard_dispatch_callbacks_t &callbacks
  ) {
    shard_dispatch_result_t result;
    if (request.shard_count == 0) {
      return result;
    }

    const auto batch_size = std::max<std::size_t>(request.batch_size, 1);
    std::size_t next_shard_to_send = 0;

    for (std::size_t index = 0; index < request.shard_count; ++index) {
      if (callbacks.prepare_shard) {
        callbacks.prepare_shard(index);
      }

      const auto pending = index - next_shard_to_send + 1;
      const auto block_ended = index + 1 == request.shard_count;
      if (pending < batch_size && !block_ended) {
        continue;
      }

      // Pace before every batch, including the first of the block, so the
      // trailing batch of the previous block is accounted for.
      const auto pacing_wait = pacer.wait_before_batch();
      result.pacer_time += pacing_wait.waited;
      if (pacing_wait.deadline_expired) {
        result.deadline_expired = true;
        break;
      }

      const auto send_started = dispatch_clock_t::now();
      const auto processed = callbacks.send_batch ?
                               callbacks.send_batch(next_shard_to_send, pending) :
                               pending;
      const auto send_finished = dispatch_clock_t::now();
      result.send_time += send_finished - send_started;

      if (processed == 0) {
        // Nothing left the host for this batch, so the block stops here and
        // the remaining shards stay unsent.
        result.deadline_expired = true;
        break;
      }

      if (!result.first_sent_at) {
        result.first_sent_at = send_started;
      }
      result.last_sent_at = send_finished;

      // Shards are ordered data-first, so the split is exact.
      const auto batch_data_end = std::min(
        next_shard_to_send + processed,
        request.data_shard_count
      );
      const auto batch_data_start = std::min(
        next_shard_to_send,
        request.data_shard_count
      );
      const auto batch_data_packets = batch_data_end - batch_data_start;

      if (callbacks.on_batch_sent) {
        callbacks.on_batch_sent(
          next_shard_to_send,
          processed,
          batch_data_packets,
          send_finished
        );
      }

      pacer.record_batch(processed);

      result.shards_sent += processed;
      result.data_shards_sent += batch_data_packets;
      result.repair_shards_sent += processed - batch_data_packets;
      next_shard_to_send += processed;

      if (processed < pending) {
        // A fallback path stopped partway through the batch.
        result.deadline_expired = true;
        break;
      }
    }

    result.wire_bytes_sent = result.shards_sent * request.wire_bytes_per_shard;
    return result;
  }

}  // namespace stream::dispatch
