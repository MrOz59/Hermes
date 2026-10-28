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

  // UAPI 15 origin: the exporter name sits in a fixed char array that need not
  // be NUL-terminated when the name fills it.
  constexpr char full[16] = {'n', 'v', 'i', 'd', 'i', 'a', '_', 'd', 'r', 'm', '_', 'x', 'x', 'x', 'x', 'x'};
  static_assert(exporter_name(full, sizeof(full)).size() == 16);
  static_assert(exporter_name("nvidia_drm\0\0\0\0\0", 16) == "nvidia_drm");
  static_assert(exporter_name("", 16).empty());
  static_assert(nvidia_exporter("nvidia_drm") && nvidia_exporter("nvidia"));
  static_assert(!nvidia_exporter("amdgpu") && !nvidia_exporter("nvidi") && !nvidia_exporter(""));

  constexpr scanout_t nvidia_linear {linear, origin_e::imported, "nvidia_drm"};
  constexpr scanout_t amd_linear {linear, origin_e::imported, "amdgpu"};
  constexpr scanout_t unnamed_linear {linear, origin_e::imported, ""};
  constexpr scanout_t dumb_linear {linear, origin_e::dumb, ""};
  constexpr scanout_t legacy_linear {linear};
  constexpr scanout_t nvidia_tiled {block_linear, origin_e::imported, "nvidia_drm"};

  // A linear scanout NVIDIA exported is GPU-origin: auto imports it now.
  static_assert(nvidia_gpu_origin(nvidia_linear));
  static_assert(route_for(dmabuf_mode_e::automatic, false, nvidia_linear) == route_e::dmabuf_import);
  // Another vendor's buffer (hybrid laptop, iGPU compositing) is not NVIDIA's
  // own import, and an unnamed exporter proves nothing: both keep the copy.
  static_assert(route_for(dmabuf_mode_e::automatic, false, amd_linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::automatic, false, unnamed_linear) == route_e::cpu_copy);
  // Dumb buffers and pre-UAPI-15 drivers keep the old rule.
  static_assert(route_for(dmabuf_mode_e::automatic, false, dumb_linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::automatic, false, legacy_linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::automatic, false, nvidia_tiled) == route_e::dmabuf_import);
  // Off and FORCE_CPU_COPY are unaffected by origin.
  static_assert(route_for(dmabuf_mode_e::off, false, nvidia_linear) == route_e::cpu_copy);
  static_assert(route_for(dmabuf_mode_e::automatic, true, nvidia_linear) == route_e::cpu_copy);
  // Origin changes rebuild capture just like layout changes, e.g. KWin falling
  // back from LinearDmabuf to a dumb buffer on the same linear modifier.
  static_assert(must_switch(dmabuf_mode_e::automatic, false, route_e::dmabuf_import, dumb_linear));
  static_assert(must_switch(dmabuf_mode_e::automatic, false, route_e::cpu_copy, nvidia_linear));
  static_assert(!must_switch(dmabuf_mode_e::automatic, false, route_e::dmabuf_import, nvidia_linear));

  std::puts("Hermes-KMS NVENC route: modes, GPU-origin layouts, scanout origin and switching PASS");
  return 0;
}
