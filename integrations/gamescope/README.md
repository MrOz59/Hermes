# Gamescope for isolated Hermes sessions

Hermes packages a patched Gamescope for its ordinary isolated application path.
The distro Gamescope remains available for nested/desktop launches. No wrapper,
PATH override, or separate opt-in launcher is needed.

The combined `gbm-scanout-hermes.patch` applies to Valve Gamescope commit
`2d217a16c7e5b56c7417257279bf102320cff024`. It incorporates NightHammer's GBM
scanout allocation series, independent KMS-device selection, and the Hermes
adaptations described in `gbm-scanout-provenance.md`.

The isolated launch must use:

- `/usr/libexec/hermes/gamescope`, with the session's explicit `--drm-device`.
- `gamescope_drm_gbm_scanout=1`, `--force-composition`, and
  `gamescope_drm_cursor_plane=0` so the primary frame includes the game and cursor.
- `--hdr-enabled` when the client session requests HDR, with matching
  `DXVK_HDR=1` and `PROTON_ENABLE_HDR=1`.
- The matching WSI manifest directory
  `/usr/share/hermes-gamescope/vulkan/implicit_layer.d`, added to Vulkan's implicit
  layer search for the isolated session while retaining inherited search paths.

The x86_64 manifest's library path is
`/usr/lib64/hermes-gamescope/libVkLayer_FROG_gamescope_wsi_x86_64.so`.
The watchdog helper is resolved through its configured absolute install path.
No additional `LD_LIBRARY_PATH` is needed with the declared runtime dependencies.
Default Lua scripts and looks are installed under
`/usr/share/hermes-gamescope/gamescope/`.

## Reproducing the build

Install Gamescope's normal build dependencies, including libgbm >= 21.3. Fetch
and initialize the pinned source and its submodules, then apply the combined
patch before configuring:

```sh
git clone https://github.com/ValveSoftware/gamescope.git gamescope-source
git -C gamescope-source checkout 2d217a16c7e5b56c7417257279bf102320cff024
git -C gamescope-source submodule update --init --recursive
git -C gamescope-source apply /absolute/path/to/gbm-scanout-hermes.patch
meson setup gamescope-build gamescope-source \
  --prefix=/usr --bindir=libexec/hermes --libdir=lib64/hermes-gamescope \
  --datadir=share/hermes-gamescope \
  -Dforce_fallback_for=libliftoff,vkroots,libdisplay-info \
  -Ddrm_backend=enabled -Dpipewire=disabled -Drt_cap=disabled \
  -Dsdl2_backend=disabled -Davif_screenshots=disabled \
  -Dinput_emulation=disabled -Denable_openvr_support=false \
  -Denable_tests=false -Dbenchmark=disabled
meson compile -C gamescope-build
DESTDIR=/absolute/path/to/package-stage \
  meson install -C gamescope-build --no-rebuild --skip-subprojects
```

The local workspace build uses already extracted dependencies and populated
subprojects; `--wrap-mode=nodownload` skips an unrelated optional v4l-utils test
reference. The package stage is `build/gamescope-gbm/package-stage` and can be
included in the Hermes RPM. Runtime packages must satisfy the Gamescope and WSI
shared-library dependencies.

## Current validation and limits

Gamescope and its x86_64 WSI layer build successfully. Staged `--help` runs and
shared-library checks find no unresolved dependencies on the development host.
No live isolated session has been verified by these checks. The known atomic
flip issue remains unchanged; successful buffer allocation does not prove
working presentation. Keep Hermes-KMS's default LINEAR modifier advertisement
with NVIDIA's current CPU capture path. The GBM change adds no CPU copy and does
not remove that existing capture step. This build has no 32-bit WSI layer.
