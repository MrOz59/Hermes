/**
 * @file tests/unit/platform/test_hermes_cursor.cpp
 * @brief Regression tests for Hermes-KMS ARGB cursor composition.
 */
#include "../../tests_common.h"

#include <cstdint>
#include <cstring>
#include <drm_fourcc.h>
#include <src/platform/common.h>
#include <vector>

namespace platf::kms {
  void blend_hermes_cursor_for_test(
    img_t &img,
    bool visible,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t> &pixels,
    uint32_t fourcc = DRM_FORMAT_ARGB8888
  );
}

namespace {
  void describe(platf::img_t &image, std::vector<std::uint8_t> &pixels, int width, int height) {
    image.data = pixels.data();
    image.width = width;
    image.height = height;
    image.pixel_pitch = 4;
    image.row_pitch = width * image.pixel_pitch;
  }
}  // namespace

TEST(HermesCursorComposition, HandlesOpaqueTransparentAndPremultipliedPixels) {
  std::vector<std::uint8_t> frame {
    10,
    20,
    30,
    0x7f,
    100,
    80,
    60,
    0x7f,
    11,
    22,
    33,
    0x7f,
  };
  platf::img_t image;
  describe(image, frame, 3, 1);

  const std::vector<std::uint8_t> cursor {
    1,
    2,
    3,
    255,
    50,
    25,
    10,
    128,
    0,
    0,
    0,
    0,
  };
  platf::kms::blend_hermes_cursor_for_test(image, true, 0, 0, 3, 1, cursor);

  const std::vector<std::uint8_t> expected {
    1,
    2,
    3,
    255,
    100,
    65,
    40,
    0x7f,
    11,
    22,
    33,
    0x7f,
  };
  EXPECT_EQ(frame, expected);
}

TEST(HermesCursorComposition, ClipsNegativeCoordinatesAgainstTheFrame) {
  std::vector<std::uint8_t> frame(2U * 2U * 4U, 0);
  platf::img_t image;
  describe(image, frame, 2, 2);

  const std::vector<std::uint8_t> cursor {
    1,
    0,
    0,
    255,
    2,
    0,
    0,
    255,
    3,
    0,
    0,
    255,
    4,
    0,
    0,
    255,
  };
  platf::kms::blend_hermes_cursor_for_test(image, true, -1, -1, 2, 2, cursor);

  std::vector<std::uint8_t> expected(2U * 2U * 4U, 0);
  expected[0] = 4;
  expected[3] = 255;
  EXPECT_EQ(frame, expected);
}

TEST(HermesCursorComposition, IgnoresHiddenOffscreenAndTruncatedCursors) {
  const std::vector<std::uint8_t> original {
    10,
    20,
    30,
    255,
    40,
    50,
    60,
    255,
  };
  std::vector<std::uint8_t> frame = original;
  platf::img_t image;
  describe(image, frame, 2, 1);

  const std::vector<std::uint8_t> pixel {1, 2, 3, 255};
  platf::kms::blend_hermes_cursor_for_test(image, false, 0, 0, 1, 1, pixel);
  platf::kms::blend_hermes_cursor_for_test(image, true, 2, 0, 1, 1, pixel);
  platf::kms::blend_hermes_cursor_for_test(image, true, 0, 0, 2, 1, pixel);

  EXPECT_EQ(frame, original);
}

TEST(HermesCursorComposition, PreservesTenBitPixelsAndChannelOrder) {
  for (const auto format : {DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ABGR2101010}) {
    const bool bgr = format == DRM_FORMAT_XBGR2101010 || format == DRM_FORMAT_ABGR2101010;
    const uint32_t original = 73U | (517U << 10) | (999U << 20) | 0xc0000000U;
    std::vector<std::uint8_t> frame(12);
    for (int i = 0; i < 3; ++i) {
      std::memcpy(frame.data() + 4 * i, &original, 4);
    }
    platf::img_t image;
    describe(image, frame, 3, 1);
    const std::vector<std::uint8_t> cursor {0, 0, 0, 0, 255, 0, 0, 255, 64, 32, 16, 128};
    platf::kms::blend_hermes_cursor_for_test(image, true, 0, 0, 3, 1, cursor, format);
    uint32_t pixels[3];
    std::memcpy(pixels, frame.data(), sizeof(pixels));
    EXPECT_EQ(pixels[0], original);
    EXPECT_EQ(pixels[1], 0xc0000000U | (1023U << (bgr ? 20 : 0)));
    // Independent integer reference for premultiplied cursor samples in the
    // compositor's output encoding, with rounding into each ten-bit channel.
    const unsigned blue = bgr ? 16 : 64;
    const unsigned red = bgr ? 64 : 16;
    EXPECT_EQ(pixels[2] & 1023U, (blue * 1023U + 73U * 127U + 127U) / 255U);
    EXPECT_EQ((pixels[2] >> 10) & 1023U, (32U * 1023U + 517U * 127U + 127U) / 255U);
    EXPECT_EQ((pixels[2] >> 20) & 1023U, (red * 1023U + 999U * 127U + 127U) / 255U);
    EXPECT_EQ(pixels[2] >> 30, 3U);
  }
}
