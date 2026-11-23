/**
 * @file src/annexb_filler.h
 * @brief Removing filler data units from an Annex B H.264 or HEVC frame.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace video::annexb {

  /// Which bitstream syntax a frame uses.
  enum class codec_e {
    h264,
    hevc,
  };

  namespace detail {
    /// Offset of the next 00 00 01 start code at or after `from`, or `size`.
    inline std::size_t find_start_code(const std::uint8_t *data, std::size_t from, std::size_t size) {
      while (size >= 3 && from + 3 <= size) {
        const auto *one = static_cast<const std::uint8_t *>(std::memchr(data + from + 2, 0x01, size - from - 2));
        if (!one) {
          break;
        }
        const auto at = static_cast<std::size_t>(one - data);
        if (data[at - 1] == 0 && data[at - 2] == 0) {
          return at - 2;
        }
        from = at - 1;
      }
      return size;
    }

    /// Filler data: H.264 nal_unit_type 12, HEVC nal_unit_type 38 (FD_NUT).
    inline bool is_filler(std::uint8_t header, codec_e codec) {
      return codec == codec_e::h264 ? (header & 0x1f) == 12 : ((header >> 1) & 0x3f) == 38;
    }
  }  // namespace detail

  /**
   * Remove the filler data units from an Annex B frame, in place.
   *
   * In CBR, radeonsi pads H.264 and HEVC frames with filler data up to the
   * target bitrate, and FFmpeg has no option to turn that off: a still
   * screen streams at the full bitrate, nearly all of it filler. A decoder
   * discards filler data, so the client has no use for it.
   *
   * A unit runs from the zero bytes before its start code to the zero bytes
   * before the next one. A unit never ends in a zero byte, so those zeros are
   * framing, and removing a unit with them leaves its neighbours exactly as
   * they were. The payload is escaped, so a start code only ever marks a
   * unit. Kept units move only when something before them was removed.
   *
   * @return The frame's size without filler. A frame with no filler, or
   *         with nothing but filler, is left whole and `size` is returned.
   */
  inline std::size_t strip_filler_data(std::uint8_t *data, std::size_t size, codec_e codec) {
    auto code = detail::find_start_code(data, 0, size);
    if (code == size) {
      return size;
    }

    std::size_t begin = 0;
    std::size_t kept = 0;
    bool removed = false;
    bool anything_else = false;
    while (begin < size) {
      const auto header = code + 3;
      const auto next_code = header < size ? detail::find_start_code(data, header, size) : size;
      auto next_begin = next_code;
      while (next_code < size && next_begin > header && data[next_begin - 1] == 0) {
        --next_begin;
      }

      if (header >= size || detail::is_filler(data[header], codec)) {
        removed = true;
      } else {
        if (kept != begin) {
          std::memmove(data + kept, data + begin, next_begin - begin);
        }
        kept += next_begin - begin;
        anything_else = true;
      }

      begin = next_begin;
      code = next_code;
    }

    return removed && anything_else ? kept : size;
  }

}  // namespace video::annexb
