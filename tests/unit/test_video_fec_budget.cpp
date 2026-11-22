/**
 * @file tests/unit/test_video_fec_budget.cpp
 * @brief The FEC a frame too large for the protocol's FEC blocks can keep.
 */
#include "../tests_common.h"

#include <src/video_fec_budget.h>

using stream::large_frame_fec_percentage;
using stream::max_fec_block_shards;
using stream::max_fec_blocks;

TEST(VideoFecBudget, KeepsWhatFitsInsteadOfNothing) {
  // Frames seen on an RX 9070 XT at 80 Mbps: 866 and 937 packets of 1040 bytes.
  EXPECT_EQ(large_frame_fec_percentage(866, 20, 0), 16);
  EXPECT_EQ(large_frame_fec_percentage(937, 20, 0), 8);
}

TEST(VideoFecBudget, NeverMoreThanConfigured) {
  EXPECT_EQ(large_frame_fec_percentage(800, 5, 0), 5);
  EXPECT_EQ(large_frame_fec_percentage(800, 0, 0), 0);
}

TEST(VideoFecBudget, NothingWhenNoParityFits) {
  // 1100 packets is 276 per block: more data than a block can hold.
  EXPECT_EQ(large_frame_fec_percentage(1100, 20, 0), 0);
  // 990 packets leaves room for 6 parity packets per block, under a minimum of 8.
  EXPECT_EQ(large_frame_fec_percentage(990, 20, 8), 0);
  EXPECT_EQ(large_frame_fec_percentage(0, 20, 0), 0);
}

TEST(VideoFecBudget, ParityAlwaysFitsEveryBlock) {
  for (std::size_t packets = 1; packets <= 1100; ++packets) {
    const int percentage = large_frame_fec_percentage(packets, 255, 0);
    // The largest block, as the frame is split: ceil(packets / blocks), plus one for the last.
    const std::size_t block = (packets + max_fec_blocks - 1) / max_fec_blocks + 1;
    const std::size_t parity = (block * percentage + 99) / 100;
    if (percentage > 0) {
      EXPECT_LE(block + parity, max_fec_block_shards) << packets << " packets at " << percentage << '%';
    }
  }
}
