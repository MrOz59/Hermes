# October release integration

Target: the end-of-October 2026 release. The combined candidate is
`release/october-integration`; it contains current `main`,
`experimental/isolated-session-improvements` (including the complete
libvirtualhid adapter) and `feature/pyrowave-codec`. The original feature
branches and histories remain available.

## Components

| Component | Candidate | Status / remaining gate |
| --- | --- | --- |
| Hermes | `release/october-integration` | Compile and package all three features together; hardware session validation remains required. |
| Hestia desktop | `feature/pyrowave-codec` | Updated with `master`; Linux x86_64, ARM64 and Arch packages compile with the pinned codec. |
| Hestia Android | `feature/pyrowave-codec` | Optional arm64 Vulkan decoder, Surface presentation and protocol port; enabled and disabled APK builds pass CI. |
| Android protocol | `moonlight-common-c:feature/pyrowave-android` | Based on the current Android pin, preserving microphone/input changes; GCC/Clang and sanitizer packet-loss tests pass. |
| Input library | `libvirtualhid:master` at `14ee6c4717d32b678c2983bfa6c54019b944d421` | Already promoted; Hermes still pins this exact revision. |
| Isolated compositor | `gamescope:hermes` at `76dd6750485b1f4b7a8b3171483481e0fa14d539` | Fork package and compositor CI pass; validate actual scanout with the combined host. |

PyroWave uses C API 0.5 at
`d2997ac172bdc00e29c58e3f2938acb7e94580bf` in all builds. The protocol is
identical across clients: SCM 0x40000000, format 0x01000000,
`PYROWAVE/90000`, and bitStreamFormat 64. It stays an explicit, SDR YUV 4:2:0
LAN option. Ordinary automatic codec selection remains on the existing codecs.

## Build and packaging gates

- The full host test build requires the SDK rather than silently disabling it.
- Arch, DEB and RPM CI build the same pinned SDK and require codec support.
- Bundled Hermes libraries install privately under the distribution's native
  library directory: `/usr/lib/hermes` on Arch, `/usr/lib64/hermes` on Fedora
  x86_64 and `/usr/lib/x86_64-linux-gnu/hermes` on Ubuntu amd64. Hestia uses its
  own private directory; the packages must coexist without file conflicts.
- Native Hermes packages use an absolute install RUNPATH to that private
  directory. File capabilities enable glibc secure execution, which can reject
  the old `$ORIGIN/../lib/hermes` path on Fedora and Ubuntu. AppImage/Flatpak
  retain a relative path for relocation. No global ldconfig entry is added.
- RPM and DEB CI install the finished package, verify its capabilities and
  RUNPATH, prove `AT_SECURE=1` in a codec-linked probe with the same capabilities,
  and execute `/usr/bin/hermes --help` as a non-root user in a clean environment.
  An `ldd` or root-only launch is insufficient to catch this regression.
- Hestia Linux tarballs carry the codec beside the client with a relative
  RUNPATH. AppImage CI also builds the SDK before bundling dependencies.
- Android's codec workflow checks the native library and JNI exports in the
  arm64 APK. Feature-branch nightlies include the codec; the default-branch
  signing and publication gates are preserved.
- A failed OpenGL conversion fence drops a PyroWave frame rather than allowing
  Vulkan to read an unfinished conversion. Packet tests verify independent
  frame recovery and that opaque frame lengths exclude network padding.

Build success is not evidence of a working DRM scanout, a correct mobile GPU
image, or lower latency. The hardware checks below remain separate release gates.

## Hardware validation before promotion

Use the maintainer's RX 6700 XT host and the actual Android client first. Start
at 720p60 SDR, then 1080p60 and the desired refresh rate. Capture the host and
client commit, GPU/driver, resolution/FPS/bitrate and logs for every result.

1. **Baseline:** physical desktop + legacy codec, then physical desktop +
   PyroWave on both desktop and Android clients. Check moving detail, orientation,
   colors and uninterrupted audio. Compare frame-time, dropped frames, CPU/GPU
   load, bandwidth and Android temperature against HEVC at the same settings.
2. **Virtual output:** repeat PyroWave on Hermes-KMS. The existing GPU conversion
   tests cannot certify exported buffers on the maintainer's real GPU.
3. **Isolated Gamescope:** use the packaged fork, render on the real GPU and
   scan out on the session's Hermes-KMS card. Validate Steam Big Picture,
   overlay, first frame, resize and game launch/exit.
4. **Input:** use DS4 in PlayStation and Xbox emulation; check buttons, analog
   axes, rumble and reconnect. Add a borrowed DualSense when available. Verify
   keyboard, mouse and Android S Pen. Host input must not reach the isolated
   game and client input must not reach the host desktop.
5. **Two sessions:** one client can use keyboard/mouse. Verify independent
   display, input, game audio and microphone; stopping one session must not stop
   the other or leave devices/ACLs attached to the wrong seat.
6. **Lifecycle:** disconnect/resume ten times, switch desktop/isolated apps,
   background/foreground Android and rotate the display. Check packet-loss
   recovery, stop during launch, controller hotplug and broker restart.
7. **Installed packages:** test a clean install/upgrade and rollback on the
   chosen distro. Launch the installed binaries without an SDK directory or
   custom LD_LIBRARY_PATH; confirm PyroWave is advertised only when enabled.

Record hardware results before merging the combined host or codec clients into
their main branches. If a release gate fails, fix it on these candidates and
rerun the affected check; do not treat the end-of-month target as test evidence.

Android details: https://github.com/MrOz59/Hestia-Android/blob/feature/pyrowave-codec/docs/pyrowave.md
Input procedure: [DS4/DualSense validation](libvirtualhid-experimental.md#manual-validation-with-a-ds4-and-dualsense)
Isolation procedure: [Gamescope integration](experimental-isolated-gamescope.md)
