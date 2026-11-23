/**
 * @file tests/unit/test_rtsp_surround.cpp
 * @brief The audio layouts the RTSP DESCRIBE reply advertises.
 */
#include "../tests_common.h"

#include <src/audio.h>
#include <src/rtsp.h>

TEST(RtspSurroundParams, EveryLayoutCompensatesTheClientRotation) {
  // moonlight-common-c moves the last speaker of a normal-quality surround
  // layout to index 3 and shifts the rest up, so the advertised mapping is
  // rotated the other way over that layout's own channels: 6 for 5.1, 8 for
  // 7.1. These are the values Sunshine advertises after its #6019.
  EXPECT_EQ(rtsp_stream::surround_params(audio::STEREO), "21101");
  EXPECT_EQ(rtsp_stream::surround_params(audio::HIGH_STEREO), "21101");
  EXPECT_EQ(rtsp_stream::surround_params(audio::SURROUND51), "642012453");
  EXPECT_EQ(rtsp_stream::surround_params(audio::HIGH_SURROUND51), "660012345");
  EXPECT_EQ(rtsp_stream::surround_params(audio::SURROUND71), "85301245673");
  EXPECT_EQ(rtsp_stream::surround_params(audio::HIGH_SURROUND71), "88001234567");
}
