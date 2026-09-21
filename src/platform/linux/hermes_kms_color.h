/**
 * @file src/platform/linux/hermes_kms_color.h
 * @brief Frame-associated colour state from Hermes-KMS UAPI v14.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace VDISPLAY::hermes_kms {
  constexpr std::uint64_t cap_frame_color = 1ULL << 18;
  constexpr std::uint32_t color_valid = 1U << 0;
  constexpr std::uint32_t color_hdr_valid = 1U << 1;
  constexpr std::uint32_t color_rgb_full_range = 1U << 2;
  constexpr std::uint32_t colorspace_default = 0;
  constexpr std::uint32_t colorspace_bt2020_rgb = 9;

  // Fixed-width mirror of drm_hermes_kms_frame_color. Explicit padding keeps
  // the native/compat ABI identical without depending on installed KMS headers.
  struct color_t {
    std::uint32_t flags {};
    std::uint32_t colorspace {};
    std::uint32_t metadata_type {};
    std::uint8_t eotf {};
    std::uint8_t descriptor_type {};

    struct xy_t {
      std::uint16_t x {}, y {};
    } primaries[3], white_point;

    std::uint16_t max_luminance {};
    std::uint16_t min_luminance {};
    std::uint16_t max_cll {};
    std::uint16_t max_fall {};
    std::uint16_t padding {};
    std::uint32_t reserved[2] {};

    bool known() const {
      return flags & color_valid;
    }

    bool pq() const {
      return known() && (flags & color_hdr_valid) && eotf == 2 &&
             colorspace == colorspace_bt2020_rgb;
    }

    bool supported() const {
      if (!known() || (flags & ~(color_valid | color_hdr_valid | color_rgb_full_range)) || !(flags & color_rgb_full_range) || padding || reserved[0] || reserved[1]) {
        return false;
      }
      if (colorspace != colorspace_default && colorspace != colorspace_bt2020_rgb) {
        return false;
      }
      if (!(flags & color_hdr_valid)) {
        const color_t zero {};
        return std::memcmp(&metadata_type, &zero.metadata_type, offsetof(color_t, reserved) - offsetof(color_t, metadata_type)) == 0;
      }
      return metadata_type == 0 && descriptor_type == 0 &&
             (eotf == 0 || (eotf == 2 && colorspace == colorspace_bt2020_rgb));
    }

    bool operator==(const color_t &other) const {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }

    template<class Metadata>
    bool copy_hdr_metadata(Metadata &out) const {
      out = {};
      if (!supported() || !pq()) {
        return false;
      }
      for (int i = 0; i < 3; ++i) {
        out.displayPrimaries[i].x = primaries[i].x;
        out.displayPrimaries[i].y = primaries[i].y;
      }
      out.whitePoint.x = white_point.x;
      out.whitePoint.y = white_point.y;
      out.maxDisplayLuminance = max_luminance;
      out.minDisplayLuminance = min_luminance;
      out.maxContentLightLevel = max_cll;
      out.maxFrameAverageLightLevel = max_fall;
      return true;
    }
  };

  static_assert(sizeof(color_t) == 48);
  static_assert(offsetof(color_t, metadata_type) == 8);
  static_assert(offsetof(color_t, reserved) == 40);
}  // namespace VDISPLAY::hermes_kms
