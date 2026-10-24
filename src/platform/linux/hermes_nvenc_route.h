/**
 * @file src/platform/linux/hermes_nvenc_route.h
 * @brief Which capture path feeds NVENC from a Hermes-KMS scanout.
 */
#pragma once

#include <cstdint>
#include <cstring>

namespace platf::hermes_nvenc {

  /// DRM_FORMAT_MOD_VENDOR_NVIDIA, kept local so this header has no libdrm dependency.
  constexpr std::uint64_t modifier_vendor_nvidia = 0x03;

  /**
   * HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF:
   * - unset/"0": CPU copy (the validated default);
   * - "1": always import the scanout DMA-BUF on the NVIDIA GPU;
   * - "auto": import only scanouts that must be GPU-origin, copy the rest.
   * Anything else is treated as unset, and reported as invalid.
   */
  enum class dmabuf_mode_e {
    off,
    on,
    automatic,
  };

  struct parsed_mode_t {
    dmabuf_mode_e mode {dmabuf_mode_e::off};
    bool valid {true};
  };

  inline parsed_mode_t parse_dmabuf_mode(const char *value) {
    if (!value || !*value || !std::strcmp(value, "0")) {
      return {dmabuf_mode_e::off, true};
    }
    if (!std::strcmp(value, "1")) {
      return {dmabuf_mode_e::on, true};
    }
    if (!std::strcmp(value, "auto")) {
      return {dmabuf_mode_e::automatic, true};
    }
    return {dmabuf_mode_e::off, false};
  }

  enum class route_e {
    cpu_copy,
    dmabuf_import,
  };

  /**
   * A scanout in an NVIDIA vendor layout (block-linear, Tegra tiles) was
   * allocated by an NVIDIA GPU: Hermes-KMS's own dumb buffers are always
   * linear, and the driver never retiles. That makes such a frame the one
   * case where the source is known to be GPU memory rather than the
   * system-memory dumb buffers NVIDIA's EGL import has corrupted beyond
   * ~2 MiB, and also a frame the CPU copy cannot read, since it would need
   * to detile NVIDIA's layout. A linear scanout carries no such proof: it may
   * be a dumb buffer or a linear buffer the compositor allocated on the GPU.
   */
  constexpr bool gpu_tiled(std::uint64_t modifier) {
    return (modifier >> 56) == modifier_vendor_nvidia;
  }

  /**
   * The path for a frame with this modifier. HERMES_KMS_FORCE_CPU_COPY=1
   * wins over everything, as it always has.
   */
  constexpr route_e route_for(dmabuf_mode_e mode, bool forced_cpu_copy, std::uint64_t modifier) {
    if (forced_cpu_copy) {
      return route_e::cpu_copy;
    }
    switch (mode) {
      case dmabuf_mode_e::on:
        return route_e::dmabuf_import;
      case dmabuf_mode_e::automatic:
        return gpu_tiled(modifier) ? route_e::dmabuf_import : route_e::cpu_copy;
      case dmabuf_mode_e::off:
      default:
        return route_e::cpu_copy;
    }
  }

  /**
   * Whether a running capture on `current` must be rebuilt for this frame.
   * Only automatic mode moves between paths; in the fixed modes a frame the
   * path cannot handle is that path's error to report.
   */
  constexpr bool must_switch(dmabuf_mode_e mode, bool forced_cpu_copy, route_e current, std::uint64_t modifier) {
    return mode == dmabuf_mode_e::automatic && route_for(mode, forced_cpu_copy, modifier) != current;
  }

}  // namespace platf::hermes_nvenc
