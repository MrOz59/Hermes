/**
 * @file tests/unit/platform/test_hermes_cursor.cpp
 * @brief Regression tests for Hermes-KMS ARGB cursor composition.
 */
#include "../../tests_common.h"

#include <cstdint>
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
    const std::vector<std::uint8_t> &pixels
  );

  void blend_hermes_cursor_for_test(
    img_t &img,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t> &pixels,
    std::uint32_t fourcc
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

namespace {
  /// A 10-bit frame word: `high` in bits 29:20, `mid` in 19:10, `low` in 9:0.
  constexpr std::uint32_t word10(std::uint32_t high, std::uint32_t mid, std::uint32_t low, std::uint32_t pad = 0) {
    return pad << 30 | high << 20 | mid << 10 | low;
  }

  void describe_words(platf::img_t &image, std::vector<std::uint32_t> &words, int width, int height) {
    image.data = reinterpret_cast<std::uint8_t *>(words.data());
    image.width = width;
    image.height = height;
    image.pixel_pitch = 4;
    image.row_pitch = width * image.pixel_pitch;
  }

  // Premultiplied ARGB8888, bytes B, G, R, A: opaque (R 51, B 255),
  // transparent, and half-covering (G 64 at alpha 128).
  const std::vector<std::uint8_t> ten_bit_cursor {
    255,
    0,
    51,
    255,
    9,
    9,
    9,
    0,
    0,
    64,
    0,
    128,
  };
}  // namespace

TEST(HermesCursorComposition, BlendsIntoRedHighTenBitWordsKeepingPadding) {
  // XR30: R in the high bits.
  std::vector<std::uint32_t> frame {
    word10(100, 200, 300, 3),
    word10(7, 8, 9, 3),
    word10(1000, 0, 500, 3),
  };
  platf::img_t image;
  describe_words(image, frame, 3, 1);

  platf::kms::blend_hermes_cursor_for_test(image, 0, 0, 3, 1, ten_bit_cursor, DRM_FORMAT_XRGB2101010);

  // 8-bit channels scale to ten bits: 255 -> 1023, 51 -> 205. Half cover:
  // (src * 1023 + dst * 127 + 127) / 255.
  EXPECT_EQ(frame[0], word10(205, 0, 1023, 3));
  EXPECT_EQ(frame[1], word10(7, 8, 9, 3));
  EXPECT_EQ(frame[2], word10(498, 257, 249, 3));
}

TEST(HermesCursorComposition, BlendsIntoBlueHighTenBitWords) {
  // XB30: B in the high bits.
  std::vector<std::uint32_t> frame {
    word10(0, 0, 0),
    word10(7, 8, 9),
    word10(500, 0, 1000),
  };
  platf::img_t image;
  describe_words(image, frame, 3, 1);

  platf::kms::blend_hermes_cursor_for_test(image, 0, 0, 3, 1, ten_bit_cursor, DRM_FORMAT_XBGR2101010);

  EXPECT_EQ(frame[0], word10(1023, 0, 205));
  EXPECT_EQ(frame[1], word10(7, 8, 9));
  EXPECT_EQ(frame[2], word10(249, 257, 498));
}
