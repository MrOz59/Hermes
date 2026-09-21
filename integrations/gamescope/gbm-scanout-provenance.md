# GBM scanout source and adaptation

Sources researched September 15, 2026:

- Author and independent report:
  https://forums.developer.nvidia.com/t/display-modes-above-2560x1440p-120hz-with-hdr-enabled-cause-flickering-corruption-within-gamescope-session/295314?page=2
  NightHammer posted the experiment August 20; a second user reported clean
  5120x1440 output on RTX 4080 / NVIDIA 610.57.04 on August 27. These are reports
  from other hardware, not validation of this Hermes-KMS integration.
- Exact author's source:
  https://github.com/NightHammer1000/gamescope/tree/2bfc18c736520b7d4f9756977213ea439daa1c63
- Author series base: `17baf4abd1ab3353fb705e4d0d023f84e870f7e8`.
- Valve upstream base used here: `2d217a16c7e5b56c7417257279bf102320cff024`.

`gbm-scanout-hermes.patch` applies directly to the pinned Valve source. It retains
the series' source changes and review fixes: modifier intersection, Vulkan import
memory-type validation, image ownership and cleanup, GBM fd lifetime, and safe
runtime output-remake notification. Upstream's newer HDR-capability-change
rebuild condition remains in place.

The Hermes additions select KMS independently with `--drm-device`. When that KMS
card differs from the Vulkan GPU, GBM allocates on the selected GPU's render node;
framebuffer imports and atomic commits remain on the specified virtual KMS card.
This does not acquire a physical display card or DRM master for allocation.

Explicit `--drm-device` plus `gamescope_drm_gbm_scanout=1` requires successful GBM
main-output allocation/import. Missing capabilities or failed output import are
reported instead of silently falling back to Vulkan allocation. Small auxiliary
texture allocations retain the author's fallback. Logs identify successful main
output allocation with `GBM scanout output active`.

GBM allocates scanout-capable memory and Vulkan imports the same buffer for GPU
composition. There is no new CPU pixel copy. Ten-bit format selection, BT.2020/PQ
processing, metadata and atomic-flip synchronization policy remain unchanged.
The separate atomic-flip issue is not fixed by this patch.

Build adaptations are also included: `<cfloat>` explicitly supplies upstream's
`DBL_MAX` use on GCC 16, and Meson installs scripts/looks relative to configured
`datadir`, replacing an install script that hardcoded `/share/gamescope`. The watchdog launches `gamescopereaper` from
the configured absolute bindir, avoiding dependency on PATH or the distro helper.

The integration is packaged for ordinary isolated launches; former opt-in
wrappers and old standalone isolated patches were removed. See `README.md` for
packaged paths, reproducible build commands and current limitations. No host
configuration or global Vulkan layer search changes are required.

Vulkan implicit layer search documentation:
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md
