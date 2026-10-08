#include "session_frame.h"

#include <array>
#include <cstdlib>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
  // Offset plus padded rows, with guard bytes that must never become pixels.
  const std::array<std::uint8_t, 20> input {99, 99, 1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 9, 10, 11, 12, 13, 14, 15, 16};
  std::vector<std::uint8_t> output;
  require(platf::kwin::copy_bgrx(output, input.data(), input.size(), 2, 18, 10, 2, 2), "Padded frame rejected");
  require(output.size() == 16 && output[0] == 1 && output[7] == 8 && output[8] == 9 && output[15] == 16, "Padding copied into pixels");
  const auto previous = output;
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 21, 0, 8, 2, 2), "Out-of-bounds offset accepted");
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 2, 19, 8, 2, 2), "Out-of-bounds chunk accepted");
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 2, 17, 10, 2, 2), "Truncated last row accepted");
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 0, 20, 7, 2, 2), "Short stride accepted");
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 0, 20, -8, 2, 2), "Unsupported negative stride accepted");
  require(!platf::kwin::copy_bgrx(output, nullptr, 20, 0, 20, 8, 2, 2), "Null mapping accepted");
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 0, 20, 8, 0, 2), "Empty frame accepted");
  require(!platf::kwin::copy_bgrx(output, input.data(), input.size(), 0, 20, 8, UINT32_MAX, UINT32_MAX), "Oversized frame accepted");
  require(output == previous, "Rejected frame modified destination");
  require(platf::kwin::copy_bgrx(output, input.data(), input.size(), 2, 8, 8, 2, 1), "Single row rejected");
  std::cout << "SESSION FRAME CHECK PASS\n";
}
