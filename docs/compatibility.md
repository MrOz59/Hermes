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
| Weston | none — see below | Driver verified, activation N/A |
| GNOME / Mutter | `org.gnome.Mutter.DisplayConfig` | Fix landed, unconfirmed |
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

### GNOME / Mutter — fix landed, unconfirmed

Mutter implements neither `kscreen-doctor` nor `wlr-output-management`, only its
own `org.gnome.Mutter.DisplayConfig` D-Bus interface.

This was reported broken as a black screen on the client while input and audio
kept working. Two causes, both fixed:

- Hermes asked whether Mutter had adopted the connector with a single call
  issued milliseconds after connecting it. Mutter adopts asynchronously, so the
  answer was reliably "no" and the session was declared failed.
- Mutter does adopt the connector, but at whichever mode *it* prefers. It drove
  the output at one resolution while Hermes captured at the requested one, so
  the encoder published frames the compositor never rendered.

Hermes now waits for Mutter to probe the connector and pushes the requested mode
through `ApplyMonitorsConfig`. Because that call is all-or-nothing, every config
is submitted to Mutter's `VERIFY` method first and abandoned if it does not
validate, and it is applied as *temporary* so `~/.config/monitors.xml` is never
rewritten.

The payload was validated against mutter 50.4 running headless. **It has not yet
been confirmed in a real GNOME session on real hardware.**

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
| AMD RDNA2 (Navi 2x) | VAAPI | Zero-copy DMA-BUF import | Verified |
| AMD RDNA3 (Navi 3x) | VAAPI | Zero-copy DMA-BUF import | One unexplained report, see below |
| Intel | VAAPI | Zero-copy DMA-BUF import | Expected to work |
| NVIDIA | NVENC | CPU copy | Contributed, see below |
| any | software | either | Fallback |

The zero-copy path is validated with `XRGB8888`, linear. NV12/P010 and HDR are
not validated.

### An unexplained DMA-BUF import failure on RDNA3

One report ([#22](https://github.com/MrOz59/Hermes/issues/22)) has
`eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` refusing the Hermes-KMS scanout buffer
with `EGL_BAD_ALLOC` on an RX 7800 XT (Navi 32, RDNA3), leaving the client with
a black image while input and audio work. The same import succeeds on an
RX 6700 XT (Navi 22, RDNA2).

**This is a single report and RDNA3 is not established as the cause.** It is
recorded here because everything else that could plausibly differ has been
tested and ruled out, not because the GPU generation has been proven guilty:

- the buffer is byte-for-byte identical on both machines — `1600x1068`,
  `XR24`, modifier `0x0` (linear), pitch 6400, DMA-BUF 6836224 bytes;
- Hermes-KMS 0.3.0 and 0.3.1 produce the same buffer, down to the same CRC;
- kernels 7.0.9 and 7.1.8 produce the same buffer and the same successful CPU
  mapping of it;
- Mesa 26.1.0 and 26.1.6 both import it successfully on RDNA2;
- `EGL_EXT_image_dma_buf_import_modifiers` is present on both, so both sides
  pass the same attributes.

Two things are still uncontrolled. The reporter runs CachyOS's Mesa build
(`3:26.1.6-1`), while the comparison used Arch's (`1:26.1.6-1`) — same upstream
version, different packaging. And the isolated import checker has not yet been
run to completion on the affected machine, so the failure is currently only
observed through Hermes, not reproduced standalone.

If you have an RDNA3 card, a data point either way is genuinely useful — see
[Reporting problems and contributing](#reporting-problems-and-contributing).

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
| `wlgrab` | `wlr-screencopy-unstable-v1` | wlroots, KWin — **not COSMIC** |
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

**Portrait modes above 2160 px tall are rejected.** The driver caps geometry at
`HERMES_KMS_MAX_WIDTH` 3840 by `HERMES_KMS_MAX_HEIGHT` 2160, so a client asking
for a portrait mode such as 1440x2560 fails validation with `-EINVAL`. This
affects tablets and phones streaming in portrait orientation.

**Exclusive mode is KDE and wlroots only.** See
[GNOME / Mutter](#gnome--mutter--fix-landed-unconfirmed).

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
