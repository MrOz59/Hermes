// A GPU-free regression for choosing the Hermes-KMS capture path for NVENC.
// Compile with -std=c++17 -I. and run directly (also exercised by branch CI).
#include "src/platform/linux/hermes_nvenc_route.h"

#include <cassert>
#include <cstdio>

int main() {
  using namespace platf::hermes_nvenc;

  constexpr std::uint64_t linear = 0;
  constexpr std::uint64_t invalid = 0x00ffffffffffffffULL;
  // DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c=0, s=1, g=2, k=0xfe, h=4), a
  // typical 32-bpp Turing+ layout, and the GB20x 8-bpp sector layout (s=2).
  constexpr std::uint64_t block_linear = 0x0300000000000000ULL | 0x10 | 4 | (0xfeULL << 12) | (2ULL << 20) | (1ULL << 22);
  constexpr std::uint64_t gb20x_8bpp = 0x0300000000000000ULL | 0x10 | 4 | (0xfeULL << 12) | (2ULL << 20) | (1ULL << 26);
  constexpr std::uint64_t tegra_tiled = 0x0300000000000001ULL;
  constexpr std::uint64_t amd_tiled = 0x0200000000000001ULL;

  static_assert(!gpu_tiled(linear));
  static_assert(!gpu_tiled(invalid));
  static_assert(gpu_tiled(block_linear));
  static_assert(gpu_tiled(gb20x_8bpp));
  static_assert(gpu_tiled(tegra_tiled));
  static_assert(!gpu_tiled(amd_tiled));

  assert(parse_dmabuf_mode(nullptr).mode == dmabuf_mode_e::off);
  assert(parse_dmabuf_mode("").mode == dmabuf_mode_e::off);
  assert(parse_dmabuf_mode("0").mode == dmabuf_mode_e::off);
  assert(parse_dmabuf_mode("1").mode == dmabuf_mode_e::on);
  assert(parse_dmabuf_mode("auto").mode == dmabuf_mode_e::automatic);
  const auto bogus = parse_dmabuf_mode("yes");
  assert(bogus.mode == dmabuf_mode_e::off && !bogus.valid);
  assert(parse_dmabuf_mode("auto").valid && parse_dmabuf_mode(nullptr).valid);

  // Default: always the CPU copy, whatever the scanout layout.
  static_assert(route_for(dmabuf_mode_e::off, false, linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::off, false, block_linear) == route_e::cpu_copy);
  // "1": always the import, as before this header existed.
  static_assert(route_for(dmabuf_mode_e::on, false, linear) == route_e::dmabuf_import);
  // "auto": import exactly the frames that must be GPU-origin.
  static_assert(route_for(dmabuf_mode_e::automatic, false, block_linear) == route_e::dmabuf_import);
  static_assert(route_for(dmabuf_mode_e::automatic, false, linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::automatic, false, invalid) == route_e::cpu_copy);
  // HERMES_KMS_FORCE_CPU_COPY=1 still wins over every mode.
  static_assert(route_for(dmabuf_mode_e::on, true, block_linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::automatic, true, block_linear) == route_e::cpu_copy);

  // Only automatic mode rebuilds capture when the layout changes underneath it.
  static_assert(must_switch(dmabuf_mode_e::automatic, false, route_e::cpu_copy, block_linear));
  static_assert(must_switch(dmabuf_mode_e::automatic, false, route_e::dmabuf_import, linear));
  static_assert(!must_switch(dmabuf_mode_e::automatic, false, route_e::dmabuf_import, block_linear));
  static_assert(!must_switch(dmabuf_mode_e::automatic, false, route_e::cpu_copy, linear));
  static_assert(!must_switch(dmabuf_mode_e::on, false, route_e::dmabuf_import, linear));
  static_assert(!must_switch(dmabuf_mode_e::off, false, route_e::cpu_copy, block_linear));
  static_assert(!must_switch(dmabuf_mode_e::automatic, true, route_e::cpu_copy, block_linear));

  std::puts("Hermes-KMS NVENC route: modes, GPU-origin layouts and switching PASS");
  return 0;
}
