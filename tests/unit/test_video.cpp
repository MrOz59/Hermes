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
