/**
 * @file tests/unit/platform/test_cuda_pixel.cpp
 * @brief Host-side checks of the arithmetic the CUDA RGB to NV12/P010 kernel performs.
 */
#include "../../tests_common.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <src/platform/linux/cuda_pixel.h>
#include <src/video_colorspace.h>

namespace {
  using namespace cuda::pixel;

  static_assert(sizeof(matrix_t) == sizeof(video::color_t), "the kernel reads video::color_t as matrix_t");

  matrix_t codes(video::colorspace_e space, bool full_range, unsigned bits) {
    matrix_t matrix;
    std::memcpy(&matrix, video::new_color_vectors_from_colorspace({space, full_range, bits}), sizeof(matrix));
    return matrix;
  }

  struct yuv_t {
    std::uint32_t y, u, v;
  };

  /// What the kernel writes for a pixel whose chroma block is uniform.
  yuv_t convert(const matrix_t &matrix, rgb_t pixel, unsigned bits) {
    const std::uint32_t max_code = (1u << bits) - 1;
    return {
      quantize(encode(matrix.y, pixel), max_code),
      quantize(encode(matrix.u, pixel), max_code),
      quantize(encode(matrix.v, pixel), max_code),
    };
  }

  constexpr std::uint32_t word10(std::uint32_t high, std::uint32_t mid, std::uint32_t low, std::uint32_t pad = 0) {
    return pad << 30 | high << 20 | mid << 10 | low;
  }
}  // namespace

TEST(CudaPixel, UnpacksEveryLayout) {
  const float half = 512 / 1023.0f;

  const auto rgb10 = unpack(layout_e::rgb10, word10(1023, 512, 0));
  EXPECT_FLOAT_EQ(rgb10.r, 1.0f);
  EXPECT_FLOAT_EQ(rgb10.g, half);
  EXPECT_FLOAT_EQ(rgb10.b, 0.0f);

  const auto bgr10 = unpack(layout_e::bgr10, word10(0, 512, 1023));
  EXPECT_FLOAT_EQ(bgr10.r, 1.0f);
  EXPECT_FLOAT_EQ(bgr10.g, half);
  EXPECT_FLOAT_EQ(bgr10.b, 0.0f);

  // Bytes B 0x80, G 0xff, R 0x00, X 0xff.
  const auto bgra8 = unpack(layout_e::bgra8, 0xff00ff80u);
  EXPECT_FLOAT_EQ(bgra8.r, 0.0f);
  EXPECT_FLOAT_EQ(bgra8.g, 1.0f);
  EXPECT_FLOAT_EQ(bgra8.b, 128 / 255.0f);
}

TEST(CudaPixel, PaddingBitsAreIgnored) {
  const auto plain = unpack(layout_e::rgb10, word10(10, 20, 30));
  const auto padded = unpack(layout_e::rgb10, word10(10, 20, 30, 3));
  EXPECT_FLOAT_EQ(plain.r, padded.r);
  EXPECT_FLOAT_EQ(plain.g, padded.g);
  EXPECT_FLOAT_EQ(plain.b, padded.b);
}

TEST(CudaPixel, BlackWhiteAndNeutralChromaAreExact) {
  for (const auto space : {video::colorspace_e::rec601, video::colorspace_e::rec709, video::colorspace_e::bt2020}) {
    for (const bool full_range : {false, true}) {
      for (const unsigned bits : {8u, 10u}) {
        SCOPED_TRACE(testing::Message() << "colorspace " << static_cast<int>(space) << (full_range ? " full" : " limited") << ' ' << bits << "-bit");
        const auto matrix = codes(space, full_range, bits);
        const std::uint32_t scale = 1u << (bits - 8);
        const std::uint32_t max_code = (1u << bits) - 1;

        const auto black = convert(matrix, {0, 0, 0}, bits);
        const auto white = convert(matrix, {1, 1, 1}, bits);

        EXPECT_EQ(black.y, full_range ? 0u : 16u * scale);
        EXPECT_EQ(white.y, full_range ? max_code : 235u * scale);
        EXPECT_EQ(black.u, 128u * scale);
        EXPECT_EQ(black.v, 128u * scale);
        EXPECT_EQ(white.u, 128u * scale);
        EXPECT_EQ(white.v, 128u * scale);
      }
    }
  }
}

TEST(CudaPixel, FullRangeTenBitGrayKeepsEveryInputCode) {
  // The point of converting 10-bit scanout in CUDA: all 1024 input codes
  // survive into P010 instead of collapsing to 256 through an 8-bit texture.
  const auto matrix = codes(video::colorspace_e::bt2020, true, 10);
  for (std::uint32_t code = 0; code <= 1023; ++code) {
    const float value = code / 1023.0f;
    const auto word = word10(code, code, code);
    const auto pixel = unpack(layout_e::rgb10, word);
    ASSERT_FLOAT_EQ(pixel.r, value);
    ASSERT_EQ(convert(matrix, pixel, 10).y, code) << "input code " << code;
  }
}

TEST(CudaPixel, LimitedRangeTenBitRampIsMonotonic) {
  const auto matrix = codes(video::colorspace_e::rec709, false, 10);
  std::uint32_t previous = 0;
  std::set<std::uint32_t> distinct;
  for (std::uint32_t code = 0; code <= 1023; ++code) {
    const auto y = convert(matrix, unpack(layout_e::bgr10, word10(code, code, code)), 10).y;
    EXPECT_GE(y, previous) << "input code " << code;
    previous = y;
    distinct.insert(y);
  }
  EXPECT_EQ(*distinct.begin(), 64u);
  EXPECT_EQ(*distinct.rbegin(), 940u);
  EXPECT_EQ(distinct.size(), 877u) << "every limited-range luma code from 64 to 940";
}

TEST(CudaPixel, SaturatedColoursStayInsideTheCodeRange) {
  for (const unsigned bits : {8u, 10u}) {
    const auto matrix = codes(video::colorspace_e::bt2020, false, bits);
    const std::uint32_t scale = 1u << (bits - 8);
    for (const rgb_t colour : {rgb_t {1, 0, 0}, rgb_t {0, 1, 0}, rgb_t {0, 0, 1}, rgb_t {1, 1, 0}, rgb_t {0, 1, 1}, rgb_t {1, 0, 1}}) {
      const auto yuv = convert(matrix, colour, bits);
      EXPECT_GE(yuv.y, 16u * scale);
      EXPECT_LE(yuv.y, 235u * scale);
      EXPECT_GE(yuv.u, 16u * scale);
      EXPECT_LE(yuv.u, 240u * scale);
      EXPECT_GE(yuv.v, 16u * scale);
      EXPECT_LE(yuv.v, 240u * scale);
    }
  }
}

TEST(CudaPixel, P010KeepsTheCodeInTheHighTenBits) {
  EXPECT_EQ(store<std::uint16_t>(0), 0u);
  EXPECT_EQ(store<std::uint16_t>(64), 64u << 6);
  EXPECT_EQ(store<std::uint16_t>(940), 940u << 6);
  EXPECT_EQ(store<std::uint16_t>(1023), 0xffc0u);
  EXPECT_EQ(store<std::uint8_t>(235), 235u);
}

TEST(CudaPixel, QuantizeTruncatesClampsAndRejectsNaN) {
  EXPECT_EQ(quantize(-3.0f, 1023), 0u);
  EXPECT_EQ(quantize(std::numeric_limits<float>::quiet_NaN(), 1023), 0u);
  EXPECT_EQ(quantize(64.99f, 1023), 64u);
  EXPECT_EQ(quantize(1023.4f, 1023), 1023u);
  EXPECT_EQ(quantize(5000.0f, 1023), 1023u);
  EXPECT_EQ(quantize(300.0f, 255), 255u);
}

TEST(CudaPixel, ChromaIsTheAverageOfItsBlock) {
  const auto average_pixel = average({1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {0, 0, 0});
  EXPECT_FLOAT_EQ(average_pixel.r, 0.25f);
  EXPECT_FLOAT_EQ(average_pixel.g, 0.25f);
  EXPECT_FLOAT_EQ(average_pixel.b, 0.0f);

  const auto blended = mix({0, 0, 0}, {1, 0.5f, 0.25f}, 0.5f);
  EXPECT_FLOAT_EQ(blended.r, 0.5f);
  EXPECT_FLOAT_EQ(blended.g, 0.25f);
  EXPECT_FLOAT_EQ(blended.b, 0.125f);
}
