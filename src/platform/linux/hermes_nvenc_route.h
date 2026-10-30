/**
 * @file src/platform/linux/hermes_nvenc_route.h
 * @brief Which capture path feeds NVENC from a Hermes-KMS scanout.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

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
   * Where a scanout's storage came from, as Hermes-KMS reports it
   * (HERMES_KMS_CAP_FRAME_ORIGIN, UAPI 15).
   */
  enum class origin_e {
    unknown,  ///< The driver predates UAPI 15; only the layout is known.
    dumb,  ///< One of Hermes-KMS's own system-memory dumb buffers.
    imported,  ///< PRIME-imported from another device's DMA-BUF.
  };

  /**
   * The exporter name in a fixed-size UAPI char array, which need not be
   * NUL-terminated when the name fills it.
   */
  constexpr std::string_view exporter_name(const char *name, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && name[length]) {
      ++length;
    }
    return {name, length};
  }

  /// NVIDIA's DRM module exports as "nvidia_drm"; a built-in or core export as "nvidia".
  constexpr bool nvidia_exporter(std::string_view exporter) {
    return exporter.substr(0, 6) == "nvidia";
  }

  /**
   * What the router knows about one scanout. A scanout known only by its
   * modifier converts implicitly: that is all a pre-UAPI-15 driver reports.
   * exporter views caller-owned storage and must not outlive it.
   */
  struct scanout_t {
    std::uint64_t modifier {0};
    origin_e origin {origin_e::unknown};
    std::string_view exporter {};

    constexpr scanout_t() = default;

    constexpr scanout_t(std::uint64_t layout_modifier):  // NOLINT(google-explicit-constructor)
        modifier {layout_modifier} {}

    constexpr scanout_t(std::uint64_t layout_modifier, origin_e storage_origin, std::string_view exporting_module):
        modifier {layout_modifier},
        origin {storage_origin},
        exporter {exporting_module} {}
  };

  /**
   * A scanout in an NVIDIA vendor layout (block-linear, Tegra tiles) was
   * allocated by an NVIDIA GPU: Hermes-KMS's own dumb buffers are always
   * linear, and the driver never retiles. That makes such a frame the one
   * case where the source is known to be GPU memory rather than the
   * system-memory dumb buffers NVIDIA's EGL import has corrupted beyond
   * ~2 MiB, and also a frame the CPU copy cannot read, since it would need
   * to detile NVIDIA's layout. A linear modifier carries no such proof.
   */
  constexpr bool gpu_tiled(std::uint64_t modifier) {
    return (modifier >> 56) == modifier_vendor_nvidia;
  }

  /**
   * Whether an NVIDIA GPU must have allocated this scanout: an NVIDIA layout,
   * or a buffer Hermes-KMS reports as PRIME-imported from NVIDIA's module.
   * The second case covers linear GPU-origin scanouts, which the modifier
   * alone cannot tell from a dumb buffer. Importing it on the NVIDIA GPU is
   * the import every NVIDIA Wayland compositor performs, not the
   * system-memory one. A buffer imported from another vendor (a hybrid
   * laptop compositing on the iGPU) is not, and keeps the CPU copy.
   */
  constexpr bool nvidia_gpu_origin(const scanout_t &scanout) {
    return gpu_tiled(scanout.modifier) ||
           (scanout.origin == origin_e::imported && nvidia_exporter(scanout.exporter));
  }

  /**
   * The path for this scanout. HERMES_KMS_FORCE_CPU_COPY=1 wins over
   * everything, as it always has.
   */
  constexpr route_e route_for(dmabuf_mode_e mode, bool forced_cpu_copy, const scanout_t &scanout) {
    if (forced_cpu_copy) {
      return route_e::cpu_copy;
    }
    switch (mode) {
      case dmabuf_mode_e::on:
        return route_e::dmabuf_import;
      case dmabuf_mode_e::automatic:
        return nvidia_gpu_origin(scanout) ? route_e::dmabuf_import : route_e::cpu_copy;
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
  constexpr bool must_switch(dmabuf_mode_e mode, bool forced_cpu_copy, route_e current, const scanout_t &scanout) {
    return mode == dmabuf_mode_e::automatic && route_for(mode, forced_cpu_copy, scanout) != current;
  }

}  // namespace platf::hermes_nvenc
