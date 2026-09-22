/**
 * @file src/platform/linux/cuda_pixel.h
 * @brief Pixel decoding and YUV encoding shared by the CUDA converter and its host-side tests.
 *
 * Nothing here depends on CUDA, so the arithmetic the kernels perform can be
 * checked on machines without an NVIDIA GPU.
 */
#pragma once

#include <cstdint>

#if defined(__CUDACC__)
  #define CUDA_PIXEL_FN __host__ __device__ inline
#else
  #define CUDA_PIXEL_FN inline
#endif

namespace cuda::pixel {

  /// How the 32-bit little-endian words of a CPU-copied frame hold their channels.
  enum class layout_e : int {
    bgra8,  ///< DRM XRGB8888 / ARGB8888: bytes B, G, R, X.
    rgb10,  ///< DRM XRGB2101010 / ARGB2101010: R in bits 29:20, G in 19:10, B in 9:0.
    bgr10,  ///< DRM XBGR2101010 / ABGR2101010: B in bits 29:20, G in 19:10, R in 9:0.
  };

  /// Colour channels normalized to [0, 1].
  struct rgb_t {
    float r, g, b;
  };

  CUDA_PIXEL_FN rgb_t unpack(layout_e layout, std::uint32_t word) {
    switch (layout) {
      case layout_e::rgb10:
        return {((word >> 20) & 1023u) / 1023.0f, ((word >> 10) & 1023u) / 1023.0f, (word & 1023u) / 1023.0f};
      case layout_e::bgr10:
        return {(word & 1023u) / 1023.0f, ((word >> 10) & 1023u) / 1023.0f, ((word >> 20) & 1023u) / 1023.0f};
      case layout_e::bgra8:
      default:
        return {((word >> 16) & 255u) / 255.0f, ((word >> 8) & 255u) / 255.0f, (word & 255u) / 255.0f};
    }
  }

  CUDA_PIXEL_FN rgb_t mix(rgb_t a, rgb_t b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
  }

  CUDA_PIXEL_FN rgb_t average(rgb_t a, rgb_t b, rgb_t c, rgb_t d) {
    return {(a.r + b.r + c.r + d.r) * 0.25f, (a.g + b.g + c.g + d.g) * 0.25f, (a.b + b.b + c.b + d.b) * 0.25f};
  }

  /**
   * One row of video::new_color_vectors_from_colorspace(): `dot(rgb, xyz) + w`
   * is the code value plus the 0.5 that makes truncation round.
   */
  struct vec4_t {
    float x, y, z, w;
  };

  /// Layout-compatible with video::color_t.
  struct alignas(16) matrix_t {
    vec4_t y, u, v;
    float range_y[2];
    float range_uv[2];
  };

  CUDA_PIXEL_FN float encode(const vec4_t &vec, rgb_t p) {
    return p.r * vec.x + p.g * vec.y + p.b * vec.z + vec.w;
  }

  /// Truncate a rounded code value into [0, max_code]; NaN becomes 0.
  CUDA_PIXEL_FN std::uint32_t quantize(float value, std::uint32_t max_code) {
    if (!(value > 0.0f)) {
      return 0;
    }
    if (value >= static_cast<float>(max_code)) {
      return max_code;
    }
    return static_cast<std::uint32_t>(value);
  }

  /// A sample as stored in the plane: 8-bit codes as they are, P010 codes in bits 15:6.
  template<class sample_t>
  CUDA_PIXEL_FN sample_t store(std::uint32_t code);

  template<>
  CUDA_PIXEL_FN std::uint8_t store<std::uint8_t>(std::uint32_t code) {
    return static_cast<std::uint8_t>(code);
  }

  template<>
  CUDA_PIXEL_FN std::uint16_t store<std::uint16_t>(std::uint32_t code) {
    return static_cast<std::uint16_t>(code << 6);
  }

}  // namespace cuda::pixel
