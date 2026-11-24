#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  /// Return the ABI revision of the linked Hermes Rust library.
  uint32_t hermes_rust_abi_version(void);

  /// Return the forward distance in RTP ticks modulo 2^32. The caller orders the timestamps.
  uint32_t hermes_rust_rtp_timestamp_distance(uint32_t earlier, uint32_t later);

  enum {
    HERMES_RUST_OK = 0,
    HERMES_RUST_INVALID_BUFFER = 1,
  };

  /**
   * Write forward RTP tick distances into the caller's output buffer.
   *
   * The counts must match. Equal zero counts succeed and allow null pointers.
   * Nonempty buffers must be valid and aligned for their counts, and their
   * byte ranges must not overlap. The input must be initialized and the output
   * exclusively writable for the call. Neither pointer is retained.
   *
   * Invalid counts, null, misalignment, arithmetic overflow, or overlap
   * return HERMES_RUST_INVALID_BUFFER before writing any output.
   */
  uint32_t hermes_rust_rtp_timestamp_distances(
    uint32_t earlier,
    const uint32_t *later,
    size_t later_count,
    uint32_t *distances,
    size_t distance_count
  );

#ifdef __cplusplus
}
#endif
