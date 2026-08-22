/**
 * @file tests/unit/platform/test_hermes_cursor.cpp
 * @brief Regression tests for Hermes-KMS ARGB cursor composition.
 */
#include "../../tests_common.h"

#include <cstdint>
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
    const std::vector<std::uint8_t> &pixels
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
