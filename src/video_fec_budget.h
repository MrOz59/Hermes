/**
 * @file src/video_fec_budget.h
 * @brief How much FEC a frame too large for the protocol's FEC blocks can keep.
 */
#pragma once

#include <algorithm>
#include <cstddef>

namespace stream {

  /// The video packet header has 2 bits for the FEC block count.
  constexpr std::size_t max_fec_blocks = 4;

  /// Reed-Solomon over GF(2^8): data and parity shards of one FEC block together.
  constexpr std::size_t max_fec_block_shards = 255;

  /**
   * The FEC percentage for a frame that needs more than `max_fec_blocks`
   * blocks at the configured percentage.
   *
   * Such a frame is split into `max_fec_blocks` blocks anyway. Sending it
   * with no parity at all means a single lost packet drops the frame, and
   * these frames are mostly key frames, which the frames after them need.
   * Instead keep the largest percentage whose shards still fit every block.
   * The client reads the percentage from each packet's FEC info, so it needs
   * nothing new.
   *
   * @param data_packets The frame's data packets.
   * @param configured The configured FEC percentage, the most this returns.
   * @param min_parity The client's minimum parity packets per block; the
   *        encoder raises a block's parity to it, so it has to fit too.
   * @return The percentage, or 0 when no parity fits.
   */
  constexpr int large_frame_fec_percentage(std::size_t data_packets, int configured, int min_parity) {
    if (data_packets == 0 || configured <= 0) {
      return 0;
    }
    // The frame is cut into blocks of a whole number of packets; the last
    // block can hold one more than the others.
    const std::size_t block_data = (data_packets + max_fec_blocks - 1) / max_fec_blocks + 1;
    if (block_data >= max_fec_block_shards) {
      return 0;
    }
    const std::size_t room = max_fec_block_shards - block_data;
    if (min_parity > 0 && room < static_cast<std::size_t>(min_parity)) {
      return 0;
    }
    // parity = ceil(block_data * percentage / 100) stays within `room` while
    // percentage <= room * 100 / block_data.
    const auto fits = static_cast<int>(room * 100 / block_data);
    return std::min(configured, fits);
  }

}  // namespace stream
