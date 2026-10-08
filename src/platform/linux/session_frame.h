/** @file src/platform/linux/session_frame.h
 *  @brief Bounds checking for mapped, packed BGRx capture buffers.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace platf::kwin {
  // This receiver deliberately negotiates positive-stride packed BGRx.
  // Reject unsupported layouts instead of interpreting them as that format.
  inline bool copy_bgrx(std::vector<std::uint8_t> &destination, const void *mapping, std::size_t mapping_size, std::size_t offset, std::size_t chunk_size, std::int32_t stride, std::uint32_t width, std::uint32_t height) {
    if (!mapping || !width || !height || width > 16384 || height > 16384 || stride <= 0) {
      return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    const auto row_stride = static_cast<std::size_t>(stride);
    if (row_stride < row_bytes || offset > mapping_size || chunk_size > mapping_size - offset) {
      return false;
    }
    if (height - 1 > (std::numeric_limits<std::size_t>::max() - row_bytes) / row_stride) {
      return false;
    }
    const auto required = static_cast<std::size_t>(height - 1) * row_stride + row_bytes;
    if (required > chunk_size || height > std::numeric_limits<std::size_t>::max() / row_bytes) {
      return false;
    }
    destination.resize(row_bytes * height);
    const auto *source = static_cast<const std::uint8_t *>(mapping) + offset;
    for (std::size_t y = 0; y < height; ++y) {
      std::memcpy(destination.data() + y * row_bytes, source + y * row_stride, row_bytes);
    }
    return true;
  }
}  // namespace platf::kwin
