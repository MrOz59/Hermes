# Gamescope HDR10 with Hermes-KMS

Hermes now requests Gamescope HDR for HDR client sessions, including its Steam
launcher and isolated application sessions. This closes launch integration gaps;
it is not a claim of hardware/client certification. KDE Wayland is the primary
desktop target. No host configuration is changed by these source changes.

## Paths and requirements

| Path | Implementation and requirements |
| --- | --- |
| Gamescope nested in KDE | `auto` selects `wayland`; HDR sessions add `--hdr-enabled`. Requires a Gamescope build with Wayland HDR colour management and HDR enabled on the KDE output containing its window. |
| Existing Gamescope session | Hermes launches the game directly. The owning Gamescope session must already have HDR enabled; a child environment cannot toggle its compositor. |
| Isolated application session | Uses the packaged Hermes Gamescope with GBM scanout, an explicit private DRM card, forced composition and session-requested HDR. Requires a Hermes-KMS session-device pool. |
| SDL nested backend | SDR remains available. HDR requests fail with an instruction to use Wayland or DRM. The researched Linux SDL backend does not advertise active HDR. |

The negotiated client request controls `HERMES_CLIENT_HDR`, `APOLLO_CLIENT_HDR`
and `SUNSHINE_CLIENT_HDR`, along with `DXVK_HDR` and `PROTON_ENABLE_HDR`. Both
states are assigned, so an inherited SDR override cannot persist into the next
HDR launch. The Steam helper prefers HERMES, then APOLLO, then SUNSHINE. These
variables permit game HDR; they do not prove that the compositor/output supports
it. Gamescope retains responsibility for reporting actual output capability.

Hermes does not use `--hdr-debug-force-support`, `--hdr-debug-force-output`, or
inverse tone mapping. Native game HDR and synthetic SDR expansion are different
features. Gamescope's SDR reference-white setting is left at its normal default.

## KDE setup

1. Use the matching Hermes-KMS UAPI v14 driver with `hdr_enable=1 color_depth=10`.
   Keep the output's manual/on-demand lifecycle and `initial_enabled=0`.
2. Enable HDR for that virtual output in KDE and place the Gamescope window on
   that output. Mirror layouts can put the window on another monitor; use the
   appropriate virtual-display layout or KDE window placement. A nested window
   does not bind itself to the capture output merely because dimensions match.
3. Enable HDR in Moonlight/Hestia and select Hermes's Gamescope launch mode (or
   its Steam launcher). Leave `gamescope_backend=auto` or use `wayland`.
4. Use a Gamescope package with its WSI layer, including the architecture needed
   by the game. Gamescope enables its layer for children. Enable in-game HDR and
   use a current compatible Proton version where needed. Steam must actually
   start inside this session: an already-running Steam process may handle the
   launch outside the new environment.

The output, encoder and receiving client/display must all support HDR10. The
existing [Hermes HDR pipeline](hermes_kms_hdr.md) and its AMD/NVIDIA requirements
apply. NVIDIA keeps its existing linear CPU capture plus GPU conversion/NVENC;
the deferred NVIDIA/KWin zero-copy work is not part of this integration.

## Why the capture architecture fits

Nested Gamescope uses Wayland colour descriptions for its output; KWin performs
final output composition. Hermes captures KWin's committed PQ/BT.2020 primary
plane and metadata, not Gamescope's intermediate scRGB surfaces. Gamescope may
tone-map or adapt metadata to the output. Stream the final connector metadata,
not guessed or independently copied game metadata.

Gamescope's DRM backend selects ten-bit RGB formats and commits
`Colorspace=BT2020_RGB` with PQ `HDR_OUTPUT_METADATA`. Its SDR transition uses
Default colour space with an SDR metadata blob. Both are accepted by the new
Hermes-KMS frame-colour contract. Metadata-only and format transitions still
trigger Hermes capture/encoder reinitialization before submitting changed frames.
No Gamescope-specific DRM ABI or second PQ transfer is needed.

## Validation

The CUDA-enabled Hermes application and test binary build. Two Gamescope policy
tests and six mock-launcher tests pass; they cover HDR/SDR environment transitions,
aliases, argument placement, unsupported backends and existing-session reuse.
These checks launch neither Gamescope nor Steam:

```sh
test_sunshine --gtest_filter='GamescopeHdr.*'
python3 tests/integration/test_gamescope_launch.py
```

Live nested KDE sessions remain release gates: check the actual
game HDR indicator, connector PQ metadata, SDR/HDR transitions and reconnects,
window placement, cursor appearance, encoded ten-bit colour tags and metadata,
and HDR reference images on Moonlight/Hestia. The user paused the earlier GPU
test campaign; no additional GPU/display tests were performed for this change.

## Primary sources

- [Gamescope options and HDR switches](https://github.com/ValveSoftware/gamescope/blob/2d217a16c7e5b56c7417257279bf102320cff024/src/main.cpp)
- [Wayland colour management and output capability](https://github.com/ValveSoftware/gamescope/blob/2d217a16c7e5b56c7417257279bf102320cff024/src/Backends/WaylandBackend.cpp)
- [DRM selection, ten-bit formats and HDR atomic properties](https://github.com/ValveSoftware/gamescope/blob/2d217a16c7e5b56c7417257279bf102320cff024/src/Backends/DRMBackend.cpp)
- [Explicit device handling in the session opener](https://github.com/ValveSoftware/gamescope/blob/2d217a16c7e5b56c7417257279bf102320cff024/src/wlserver.cpp)
- [SDL output HDR status](https://github.com/ValveSoftware/gamescope/blob/2d217a16c7e5b56c7417257279bf102320cff024/src/Backends/SDLBackend.cpp)

## Isolated GBM integration

The ordinary isolated application launch uses `/usr/libexec/hermes/gamescope`.
Hermes passes its assigned DRM card through `--drm-device`, enables
`gamescope_drm_gbm_scanout=1`, forces GPU composition, and composites the cursor
into the captured primary. The matching WSI manifest is added only to the session
child's environment. There is no opt-in wrapper or alternate Hermes launcher.
Nested Gamescope continues to use the host package.

The tracked patch and build instructions live in
[`integrations/gamescope`](../integrations/gamescope/README.md). Packagers must
install that runtime alongside Hermes; the isolated application profile reports
an error if it is missing. CMake paths `HERMES_GAMESCOPE_EXECUTABLE` and
`HERMES_GAMESCOPE_LAYER_DIR` describe its installed locations.

The GBM route is still experimental upstream. Compilation is verified, but
visual HDR correctness and cross-device import need a real isolated session.
The known atomic-flip problem is unchanged. The packaged WSI layer is x86_64;
a 32-bit layer is not provided by this build. The current NVIDIA CPU capture
step is unchanged; the new GBM scanout allocation itself adds no CPU pixel copy.
