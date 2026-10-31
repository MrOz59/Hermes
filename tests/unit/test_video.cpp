/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <src/video.h>

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#ifdef __linux__
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

// Verify that probing records a coherent, diagnostics-ready encoder status,
// including the per-encoder attempt list used to explain a software fallback.
struct EncoderStatusTest: PlatformTestSuite {};

TEST_F(EncoderStatusTest, ProbeRecordsAttempts) {
  if (video::probe_encoders() != 0) {
    GTEST_SKIP() << "No encoder/display available to probe";
  }

  const auto status = video::get_encoder_status();
  ASSERT_TRUE(status.probed);
  ASSERT_FALSE(status.encoder.empty());

  // The selected encoder must be the single attempt flagged `selected`, must
  // appear last, and its name must match the chosen encoder.
  ASSERT_FALSE(status.attempts.empty());
  int selected_count = 0;
  for (const auto &attempt : status.attempts) {
    ASSERT_FALSE(attempt.name.empty());
    ASSERT_FALSE(attempt.outcome.empty());
    if (attempt.selected) {
      ++selected_count;
    }
  }
  ASSERT_EQ(selected_count, 1);
  ASSERT_TRUE(status.attempts.back().selected);
  ASSERT_EQ(status.attempts.back().name, status.encoder);
  ASSERT_EQ(status.attempts.back().outcome, "selected");

  // hardware <-> "software" name, and the fallback flag is only set when a
  // hardware encoder was actually rejected before landing on software.
  ASSERT_EQ(status.hardware, status.encoder != "software");
  if (status.fell_back_to_software) {
    ASSERT_FALSE(status.hardware);
    ASSERT_GT(status.attempts.size(), 1u);
  }
}

namespace {
  video::config_t pyrowave_config(int width, int height, int framerate, int bitrate_kbps) {
    video::config_t config {};
    config.width = width;
    config.height = height;
    config.framerate = framerate;
    config.bitrate = bitrate_kbps;
    config.videoFormat = video::PYROWAVE_VIDEO_FORMAT;
    return config;
  }
}  // namespace

TEST(PyroWaveFrameBudget, SpreadsTheBitrateOverTheFrameRate) {
  // 300 Mbps at 60 fps: 5 Mbit, or 625000 bytes, per frame.
  EXPECT_EQ(video::pyrowave_frame_budget(pyrowave_config(1920, 1080, 60, 300000)), 625000u);
}

TEST(PyroWaveFrameBudget, AcceptsAFrameRateInThousandths) {
  EXPECT_EQ(
    video::pyrowave_frame_budget(pyrowave_config(1920, 1080, 60000, 300000)),
    video::pyrowave_frame_budget(pyrowave_config(1920, 1080, 60, 300000))
  );
}

TEST(PyroWaveFrameBudget, NeverExceedsAnUncompressedFrame) {
  // 2 Gbps at 30 fps would allow 8.3 MB, but a 1080p 4:2:0 frame is 3.1 MB.
  EXPECT_EQ(video::pyrowave_frame_budget(pyrowave_config(1920, 1080, 30, 2000000)), 1920u * 1080u * 3u / 2u);
}

TEST(PyroWaveFrameBudget, KeepsAFloorForTinyBitrates) {
  EXPECT_EQ(video::pyrowave_frame_budget(pyrowave_config(1920, 1080, 120, 500)), 16u * 1024u);
}

TEST(PyroWaveFrameBudget, SurvivesAMissingFrameRateOrBitrate) {
  EXPECT_EQ(video::pyrowave_frame_budget(pyrowave_config(1280, 720, 0, 0)), 16u * 1024u);
}
