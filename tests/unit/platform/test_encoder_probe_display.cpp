/**
 * @file tests/unit/platform/test_encoder_probe_display.cpp
 * @brief Test the display Hermes validates encoders on when no screen is open.
 *
 * It exists so that probing encoders at startup never creates a virtual
 * output, which would wake every monitor the compositor drives. Encoder
 * validation needs only what is checked here: the requested size, an image
 * buffer, and a synthetic frame to fill it with.
 */
#ifdef __linux__
  #include "../../tests_common.h"

  #include <src/platform/common.h>
  #include <src/video.h>

TEST(EncoderProbeDisplay, TakesTheRequestedSizeAndFillsSyntheticFrames) {
  video::config_t config {};
  config.width = 1920;
  config.height = 1080;
  config.encoder_probe = true;

  auto display = platf::encoder_probe_display(platf::mem_type_e::system, config);
  ASSERT_TRUE(display);
  EXPECT_EQ(display->width, 1920);
  EXPECT_EQ(display->height, 1080);

  auto image = display->alloc_img();
  ASSERT_TRUE(image);
  EXPECT_EQ(image->width, 1920);
  EXPECT_EQ(image->height, 1080);
  EXPECT_EQ(image->row_pitch, 1920 * 4);
  ASSERT_NE(image->data, nullptr);
  EXPECT_EQ(display->dummy_img(image.get()), 0);
  EXPECT_TRUE(display->make_avcodec_encode_device(platf::pix_fmt_e::nv12));
}

TEST(EncoderProbeDisplay, NeverCaptures) {
  video::config_t config {};
  config.width = 640;
  config.height = 480;

  auto display = platf::encoder_probe_display(platf::mem_type_e::system, config);
  ASSERT_TRUE(display);
  bool cursor = false;
  EXPECT_EQ(
    display->capture([](auto &&, bool) {
      return false;
    },
                     [](auto &) {
                       return false;
                     },
                     &cursor),
    platf::capture_e::error
  );
}

TEST(EncoderProbeDisplay, RefusesASizeOfNothing) {
  video::config_t config {};
  EXPECT_FALSE(platf::encoder_probe_display(platf::mem_type_e::system, config));
}
#endif
