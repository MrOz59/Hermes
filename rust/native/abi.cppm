module;

#include "abi.h"

#include <cstdint>
#include <span>

export module hermes.rust_interop;

export namespace hermes::rust_interop {
  [[nodiscard]] std::uint32_t abi_version() noexcept {
    return hermes_rust_abi_version();
  }

  [[nodiscard]] std::uint32_t rtp_timestamp_distance(std::uint32_t earlier, std::uint32_t later) noexcept {
    return hermes_rust_rtp_timestamp_distance(earlier, later);
  }

  enum class batch_result : std::uint32_t {
    ok = HERMES_RUST_OK,
    invalid_buffer = HERMES_RUST_INVALID_BUFFER,
  };

  /**
   * Write one wrapping RTP tick distance per input timestamp.
   *
   * The caller orders the timestamps and owns both spans. They must have
   * equal lengths and must not overlap. Empty spans succeed. Invalid lengths
   * or overlap leave output unchanged. The call borrows the spans only for
   * its duration and writes directly into the destination without allocating.
   */
  [[nodiscard]] batch_result rtp_timestamp_distances(
    std::uint32_t earlier,
    std::span<const std::uint32_t> later,
    std::span<std::uint32_t> distances
  ) noexcept {
    return static_cast<batch_result>(
      hermes_rust_rtp_timestamp_distances(earlier, later.data(), later.size(), distances.data(), distances.size())
    );
  }
}  // namespace hermes::rust_interop
