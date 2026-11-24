#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

import hermes.rust_interop;

using hermes::rust_interop::batch_result;
using hermes::rust_interop::rtp_timestamp_distance;
using hermes::rust_interop::rtp_timestamp_distances;

namespace {
  bool writes_into_borrowed_output() {
    std::array<std::uint32_t, 7> input {77, 0xfffffffe, 0xffffffff, 0, 1, 0x7ffffffe, 88};
    const auto original = input;
    std::array<std::uint32_t, 7> output {10, 99, 99, 99, 99, 99, 20};
    const std::array<std::uint32_t, 7> expected {10, 0, 1, 2, 3, 0x80000000, 20};

    const auto result = rtp_timestamp_distances(0xfffffffe, std::span {input}.subspan(1, 5), std::span {output}.subspan(1, 5));
    return result == batch_result::ok && input == original && output == expected;
  }

  bool rejects_invalid_views() {
    std::array<std::uint32_t, 4> buffer {10, 20, 30, 40};
    const auto original = buffer;
    const auto view = std::span {buffer};
    std::array<std::uint32_t, 2> output {99, 99};
    const auto untouched = output;

    // Equal-length views that share only part of their storage must fail too.
    return rtp_timestamp_distances(0, view.first(3), view.last(3)) == batch_result::invalid_buffer &&
           rtp_timestamp_distances(0, view, view) == batch_result::invalid_buffer &&
           rtp_timestamp_distances(0, view, output) == batch_result::invalid_buffer &&
           rtp_timestamp_distances(0, {}, output) == batch_result::invalid_buffer &&
           buffer == original && output == untouched;
  }
}  // namespace

int main() {
  const auto revision = hermes::rust_interop::abi_version();
  if (revision != 1) {
    std::fprintf(stderr, "Expected Hermes Rust C ABI revision 1, got %u\n", static_cast<unsigned>(revision));
    return 1;
  }
  if (rtp_timestamp_distance(100, 145) != 45 || rtp_timestamp_distance(0xfffffffe, 1) != 3) {
    std::fputs("Rust RTP timestamp distance did not handle ordinary and wrapping timestamps\n", stderr);
    return 1;
  }
  if (!writes_into_borrowed_output()) {
    std::fputs("Rust did not write the expected RTP distances into the borrowed output buffer\n", stderr);
    return 1;
  }
  if (!rejects_invalid_views()) {
    std::fputs("Rust accepted an invalid RTP buffer view or changed it on failure\n", stderr);
    return 1;
  }
  if (rtp_timestamp_distances(0, {}, {}) != batch_result::ok) {
    std::fputs("Rust rejected an empty RTP batch\n", stderr);
    return 1;
  }
}
