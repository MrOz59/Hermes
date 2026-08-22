# Compatibility and support

Hermes touches three layers that vary wildly between systems: the kernel
(virtual display driver), the compositor (which must adopt and drive that
virtual output), and the GPU stack (which must import and encode its frames). A
configuration works only when all three cooperate, so "does Hermes run on X" has
no single answer — this document records what has actually been exercised, what
shares a code path with something that has, and what is known not to work.

## Development target

**CachyOS + KDE Plasma (Wayland) + AMD.**

This is the maintainer's daily system, and it is the only configuration that is
continuously exercised while features are written. It is not a statement that
other systems matter less — it is the honest scope of what one person can keep
verified. Everything outside that target is improved as problems are reported.

## How to read this document

| Tier | Meaning |
| --- | --- |
| **Verified** | Exercised directly. The entry says how and when. |
| **Expected to work** | Shares its code path with something verified, and nothing known contradicts it. Nobody has run it. |
| **Known broken** | Reproduced, with the mechanism understood. |
| **Untested** | No information either way. |

"Expected to work" is a statement about code paths, not a promise about your
machine. If you run one of those configurations, a report either way is useful.

## Compositors

The compositor has to do two things: adopt the virtual connector when Hermes
connects it, and drive it at the mode the client negotiated. Each compositor
family exposes a different protocol for this, which is why support is per
compositor rather than per distribution.

| Compositor | Output activation | Status |
| --- | --- | --- |
| KDE Plasma / KWin (Wayland) | `kscreen-doctor` | Verified |
| COSMIC / cosmic-comp | `wlr-output-management` v4 | Verified at protocol level |
| wlroots (sway, …) | `wlr-output-management` | Expected to work |
| Hyprland / aquamarine | `wlr-output-management` | Known broken |
| Weston | none — see below | Driver verified, activation N/A |
| GNOME / Mutter | `org.gnome.Mutter.DisplayConfig` | Video verified; exclusive mode unsupported |
| gamescope | n/a (own session) | Expected to work |
| X11 | X11 capture path | Expected to work |

### KDE Plasma / KWin — verified

The development target. Output activation goes through `kscreen-doctor`, and
this is the only compositor where **exclusive mode**
(`isolated_virtual_display_option`) is fully supported: it disables the physical
outputs for the duration of the session, restores them when it ends, and
persists the previous layout so a crashed session can be recovered at next
startup.

### COSMIC / cosmic-comp — verified at protocol level

Tested 2026-08-16 against the Arch `cosmic-comp` 1:1.3.0-1 package (the binary
self-reports 1.0.0) in a disposable virtme-ng guest, driving a Hermes-KMS card
on its DRM backend.

`cosmic-comp` advertises `zwlr_output_manager_v1` **version 4** (plus
`zcosmic_output_manager_v1` v3 as an *extension* on top of it, not a
replacement). Hermes binds `min(version, 4)`, so the versions match exactly and
the existing wlroots path is used unchanged — no COSMIC-specific code exists or
is needed.

Observed:

- the virtual connector appeared in the output list with its full mode set;
- COSMIC selected the mode the client requested (`1600x1068 @ 89.99 Hz`) as
  current, because the driver marks the requested mode `DRM_MODE_TYPE_PREFERRED`;
- the compositor rendered into the card's scanout buffer, which is what Hermes'
  capture reads.

Not tested: a real streaming session, and exclusive mode. Exclusive mode uses
the same `wlr-output-management` path, so it is *expected to work*, unlike on
GNOME.

Capture on COSMIC works only with the Hermes-KMS backend — see
[Capture backends](#capture-backends).

### GNOME / Mutter — video verified

Mutter implements neither `kscreen-doctor` nor `wlr-output-management`, only its
own `org.gnome.Mutter.DisplayConfig` D-Bus interface.

This was reported broken as a black screen on the client while input and audio
kept working. The investigation found three independent problems, all fixed for
the video path:

- Hermes asked whether Mutter had adopted the connector with a single call
  issued milliseconds after connecting it. Mutter adopts asynchronously, so the
  answer was reliably "no" and the session was declared failed.
- Mutter does adopt the connector, but at whichever mode *it* prefers. It drove
  the output at one resolution while Hermes captured at the requested one, so
  the encoder published frames the compositor never rendered.
- Mutter renders on the real GPU and PRIME-imports that DMA-BUF into Hermes-KMS
  for scanout. `ACQUIRE_FRAME` incorrectly re-exported the imported GEM wrapper
  instead of returning its original DMA-BUF. The wrapper has no shmem file for
  the consumer to pin, so the attach failed with `-EINVAL`, surfaced by EGL as
  `EGL_BAD_ALLOC`. Hermes-KMS 0.3.2 returns the original DMA-BUF and carries a
  regression test for this imported-scanout path.

Hermes now waits for Mutter to probe the connector and pushes the requested mode
through `ApplyMonitorsConfig`. Because that call is all-or-nothing, every config
is submitted to Mutter's `VERIFY` method first and abandoned if it does not
validate, and it is applied as *temporary* so `~/.config/monitors.xml` is never
rewritten.

The output and scanout path was exercised on 2026-08-22 in a CachyOS VM with
kernel 7.1.8 and GNOME Shell/Mutter 50.4. Separately, the original reporter in
[#22](https://github.com/MrOz59/Hermes/issues/22) confirmed a working video
stream on an RX 7800 XT after installing the driver fix. This verifies the video
path; it does not change the exclusive-mode limitation below.

**Exclusive mode is not supported on GNOME.** There is no interface Hermes can
use to disable the physical outputs, so it logs a warning and the physical
outputs stay on. The `DisplayConfig` interface used for mode negotiation does
not provide this.

### wlroots compositors — expected to work

sway and other wlroots-based compositors expose `wlr-output-management`, the
same protocol and version verified on COSMIC, so the same Hermes code path
applies. The container image in `packaging/container` runs Hermes on a headless
sway session. They also implement `wlr-screencopy`, so the `wlgrab` capture
backend is available there in addition to Hermes-KMS.

### Hyprland — known broken

Investigated 2026-08-22 on CachyOS, kernel 7.1.8-1-cachyos, Hyprland 0.56.2-1,
aquamarine 0.14.0-2.2 and Hermes-KMS 0.3.0, from live session logs and a
controlled reproduction.

Hyprland is not a wlroots compositor. It has its own backend, **aquamarine**, so
it does not share the code path verified on sway and COSMIC — and that
difference is exactly what breaks virtual displays. The Hermes side is not the
problem: Hyprland implements `zwlr_output_manager_v1`, the protocol Hermes uses
to enable a virtual output, and it does adopt the connector when the driver
connects it, giving the virtual monitor its own workspace. The failure is a
layer below, in how aquamarine drives a secondary DRM device.

**Stock aquamarine cannot drive a virtual display at all.** In its multi-GPU
model the card holding the physical monitor becomes the primary DRM device, and
every other KMS device with an enabled output has to host its own EGL/GLES
renderer, because aquamarine renders on the primary and blits into the secondary
before scanout. A virtual display cannot host a GL renderer — Mesa has no DRI
driver for the device, so `eglInitialize` fails:

```
ERR  [EGL] eglInitialize errored out with EGL_NOT_INITIALIZED: DRI2: failed to create screen
ERR  CDRMRenderer: fail, eglInitialize failed
ERR  drm: initMgpu: no renderer
ERR  drm: Failed to initialize renderer backend for blitting
```

The connector appears and the modeset succeeds, but every commit fails at the
blit stage and no frame is ever presented — a black output while Hyprland
retries each frame. One session logged 653 blit failures and 969 `initMgpu: no
renderer`.

Exposing a render node does not help, and is what makes the failure loud rather
than silent. Hermes-KMS has one because its capture channel needs it, so
aquamarine selects it and fails at `eglInitialize` instead of skipping the
device. EVDI, which has no render node, produces the symmetric warning and is
equally unusable.

**Remove the blit and page-flip pacing fails instead.** A locally patched
aquamarine that imports the primary GPU's buffer over PRIME and scans it out
directly — the KWin/GNOME model — does present frames, and then stalls. Over a
three-minute controlled run the driver's frame sequence advanced 149 times,
about 1 fps where 60 was requested, before commits stopped being accepted at
all:

```
ERR  drm: Cannot commit when a page-flip is awaiting
```

aquamarine arms a pending flip on every buffer commit and refuses the next one
until the page-flip event arrives. Nothing in the log shows a stale event being
discarded, so either the event never reaches it, or the arm/deliver cycle
desynchronises and its scheduler never sees the flip. The leading suspect is the
driver's software vblank: `hrtimer_forward_now()` skips missed periods without
calling `drm_crtc_handle_vblank()` for each one, which under load produces
exactly the sparse flips observed. **This is not proven** — closing it needs DRM
vblank tracing alongside aquamarine at trace level.

**Smaller gaps, none fatal on their own.** The Hermes-KMS CRTC carries only a
mode, a primary plane and a cursor, so everything Hyprland commits beyond that
fails:

| Missing | Effect |
| --- | --- |
| `CTM` | Hyprland's colour management commits a matrix on *every* state commit, even the identity one, so `failed to commit ctm: no ctm prop support` repeats per commit rather than per modeset |
| `GAMMA_LUT` / `DEGAMMA_LUT` | `Couldn't get the gamma_size prop`; no gamma control, so `hyprsunset` cannot work on the virtual output |
| `VRR_CAPABLE`, `HDR_OUTPUT_METADATA`, `Colorspace`, syncobj | reported unsupported at probe — no VRR, HDR or explicit sync |
| `IN_FORMATS` | the primary plane advertises no modifier blob, so only implicit linear is available |

The synthetic EDID is rejected as well: aquamarine parses it with
libdisplay-info, which is stricter than KWin's parser, so `hyprctl monitors`
shows the virtual output with empty make, model and serial. Cosmetic, but it
applies to any libdisplay-info consumer, not only Hyprland.

Streaming an ordinary physical display should be unaffected —
`zwlr_screencopy_manager_v1` is present, so the `wlgrab` backend is available —
but no full session has been run that way.

**What would fix it.** On the aquamarine side: keep a direct import-and-scanout
path for secondary devices that cannot render, and make the flip handshake
tolerate a software vblank instead of wedging on `frameInFlight`. On the driver
side: guarantee the flip event for every commit carrying
`DRM_MODE_PAGE_FLIP_EVENT`, and add no-op `CTM` and gamma properties so the
colour pipeline stops erroring on every commit. Until then, use KDE or a wlroots
compositor, or Hermes' per-session gamescope path.

### Weston — driver verified, activation not applicable

Hermes-KMS ships a VM test that runs real Weston DRM compositors on its cards,
so the *driver* is known to work as a DRM device under Weston. Weston does not
implement `wlr-output-management`, so Hermes has no way to activate the virtual
output there — it would have to be configured by other means. Weston is used as
a driver test harness, not as a supported streaming session.

## Distributions

Distribution matters less than compositor and GPU. It determines how the kernel
module is built and how the package is installed.

| Distribution | Status | Notes |
| --- | --- | --- |
| CachyOS / Arch | Verified | Development target. In-tree PKGBUILDs. |
| Debian / Ubuntu | Expected to work | `.deb` built in CI; not exercised as a desktop. |
| Fedora | Expected to work | `.rpm` built in CI; not exercised as a desktop. |
| Bazzite / Silverblue / SteamOS | Partial | See below. |

### Image-based distributions

DKMS cannot work on ostree/bootc systems: it rebuilds the module on the running
system whenever the kernel changes, and there `/usr` is read-only at runtime, so
that moment never comes. Hermes-KMS ships a `Containerfile` that builds the
module *into* the image instead, compiling against the image's kernel and
installing it to `/usr/lib/modules/<kver>/extra`, with optional MOK signing.

Separately, `packaging/container` holds a runtime image that runs Hermes on a
headless sway session with audio and XWayland.

## GPUs and encoders

| Vendor | Encoder | Capture path | Status |
| --- | --- | --- | --- |
| AMD RDNA2 (Navi 2x) | VAAPI | Zero-copy DMA-BUF import | Verified on the development system |
| AMD RDNA3 (Navi 3x) | VAAPI | Zero-copy DMA-BUF import | Verified on RX 7800 XT (Navi 32) |
| Intel | VAAPI | Zero-copy DMA-BUF import | Expected to work |
| NVIDIA | NVENC | CPU copy | Contributed, see below |
| any | software | either | Fallback |

The zero-copy path is validated with `XRGB8888`, linear. NV12/P010 and HDR are
not validated.

### AMD zero-copy status

The RX 7800 XT failure reported in [#22](https://github.com/MrOz59/Hermes/issues/22)
was the GNOME/Mutter imported-scanout bug described above, not a GPU-generation
limitation. The decisive control imported the same geometry from a plain
`udmabuf` on the same card, kernel and Mesa stack; that succeeded both with and
without explicit modifier attributes. After Hermes-KMS returned the original
DMA-BUF, the reporter confirmed that video streaming worked.

This is direct confirmation for Navi 32, not a claim that every RDNA3 model has
been exercised. No RDNA3-specific failure is currently known, so the dedicated
hardware-testing request in [#23](https://github.com/MrOz59/Hermes/issues/23)
has been retired.

**NVIDIA needs a CPU copy.** NVIDIA's EGL import of the driver's system-memory
DMA-BUFs reads the wrong pages beyond the first ones, so NVENC sessions capture
through a CPU copy of the scanout buffer and feed the regular RAM-to-CUDA upload
path, at the cost of one `memcpy` per frame. The copy waits on the framebuffer's
exported write fence and brackets the read with `DMA_BUF_IOCTL_SYNC`. VAAPI is
unaffected and keeps the validated zero-copy import.

## Virtual display backends

### Hermes-KMS (preferred)

A purpose-built DRM/KMS driver. The compositor owns the card and scans out the
desktop; Hermes opens its **render node** and pulls each frame as a DMA-BUF. No
DRM master and no KMS access are needed, so it coexists with the compositor.

Requires the module loaded with `initial_enabled=0` and its card left on the
active seat.

### EVDI (alternative)

Capture goes through a CPU-side buffer filled by `libevdi`.

EVDI exposes **no render node** (confirmed 2026-08-16: an EVDI-only system has
`card0`/`card1` and zero `renderD*` nodes). On a normal desktop this is
irrelevant, because the compositor renders on the real GPU and treats EVDI as an
output-only device. It does mean EVDI cannot be the only DRM device in a system
— a compositor with no other GPU has nothing to render with. Hermes-KMS does
expose a render node and does not have this constraint.

An EVDI connector also reports *disconnected* until a userspace client calls
`evdi_connect` with an EDID; the card existing is not enough.

## Capture backends

| Backend | Protocol / mechanism | Works on |
| --- | --- | --- |
| Hermes-KMS | `ACQUIRE_FRAME` ioctl on the render node | Any compositor — no Wayland protocol involved |
| EVDI | `libevdi` CPU buffer | Any compositor |
| `wlgrab` | `wlr-screencopy-unstable-v1` | wlroots, KWin, Hyprland — **not COSMIC** |
| `kmsgrab` | DRM/KMS | needs DRM master or suitable permissions |
| `x11grab` | X11 | X11 sessions |

`cosmic-comp` does not implement `wlr-screencopy`; it implements
`ext-image-copy-capture-v1` instead, which Hermes does not currently support. On
COSMIC, use the Hermes-KMS backend, whose capture involves no Wayland protocol
at all and is therefore unaffected.

## Known limitations

**A stale `/etc/modprobe.d/hermes-kms.conf` silently wins.** The package installs
its default to `/usr/lib/modprobe.d/hermes-kms.conf`, but `/etc/modprobe.d`
overrides `/usr/lib/modprobe.d`. An `initial_enabled=1` written there by an older
package, or by hand, keeps the virtual connector connected at boot before Hermes
owns it, and the compositor extends the desktop onto a black output nobody is
streaming. No upgrade can fix this, because the package does not own that file.
The package reports it on install and upgrade and the driver logs a warning at
probe; neither rewrites the file. Remove it, or set `initial_enabled=0`.

**The development udev rule hides the card from every compositor.**
`udev/99-hermes-kms-ignore-seat.rules` strips `TAG+="seat"` and `ID_SEAT` from
the Hermes-KMS card so an isolated test session can claim it. With that rule
installed no compositor adopts the virtual output — KWin, Hyprland or any other
— which is usually reported as "the virtual display never appears at all". The
rule is for isolated testing only; remove it for streaming.

**Portrait modes above 2160 px tall are rejected.** The driver caps geometry at
`HERMES_KMS_MAX_WIDTH` 3840 by `HERMES_KMS_MAX_HEIGHT` 2160, so a client asking
for a portrait mode such as 1440x2560 fails validation with `-EINVAL`. This
affects tablets and phones streaming in portrait orientation.

**Exclusive mode is KDE and wlroots only.** See
[GNOME / Mutter](#gnome--mutter--video-verified). On Hyprland the
question does not arise yet — see [Hyprland](#hyprland--known-broken).

**Confined packaging may lose output management.** COSMIC filters privileged
protocol globals by Wayland security context; clients without a security context
are trusted. Hermes running as a normal user service qualifies, but a Flatpak or
Snap build could be filtered out and would not see `wlr-output-management`.

**COSMIC mirroring reports an enabled output without a current mode**
([pop-os/cosmic-comp#945](https://github.com/pop-os/cosmic-comp/issues/945)),
which may affect the layout Hermes saves before entering exclusive mode.

## Reporting problems and contributing

Keeping several distributions, compositors and GPU stacks working is not
something one maintainer can do alone, and most of what is listed above as
"expected to work" only becomes "verified" when someone runs it and says what
happened. **Issues and pull requests are very welcome** — including reports that
something simply works, which are as useful as bug reports for moving an entry
between tiers.

A useful report includes:

- distribution, kernel, compositor and its version, GPU and driver;
- `XDG_CURRENT_DESKTOP` and `XDG_SESSION_TYPE`;
- the Hermes log for the failing session;
- `cat /sys/class/drm/card*/status` and, if the module is loaded,
  `cat /sys/module/hermes_kms/parameters/initial_enabled`;
- for display-adoption problems, the compositor's view of its outputs
  (`kscreen-doctor -o`, `wlr-randr`, or Mutter's `GetCurrentState` over D-Bus).
