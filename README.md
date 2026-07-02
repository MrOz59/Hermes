# Hermes

Hermes is an Apollo-derived Linux game-streaming host focused on making
Moonlight/Hestia streaming less manual and more reliable on CachyOS/Arch,
with low-latency real virtual displays through Hermes-KMS.

Hermes-KMS is a purpose-built DRM/KMS virtual display driver that streams the
compositor's scanout straight into the hardware encoder as a DMA-BUF, with no
CPU readback. It is the default backend because it avoids the GPU→RAM→GPU copy
that EVDI does, which lowers latency. EVDI is still fully supported and can be
selected at any time — both backends work. (Measured capture cost on KWin at
720p: ~8 us/frame on Hermes-KMS vs ~180 us/frame for EVDI's CPU copy, and
constant regardless of resolution.)

Hermes keeps protocol compatibility with Sunshine, Moonlight, Artemis, and
Hestia. The normal GameStream/Sunshine flow remains the fallback path, while
Hestia can use Hermes protocol extensions when the host reports support for
them. The product is branded Hermes throughout the UI, but the protocol lineage
identifier stays `Apollo` so existing Artemis/Hestia clients keep working
unchanged.

## Current focus

- Create and activate a real virtual display. Hermes-KMS is the default for
  its lower latency; EVDI is a fully supported alternative and the automatic
  choice where Hermes-KMS is unavailable.
- Use KDE/KScreen or Wayland output-management integration to make the
  compositor actually render into the virtual display.
- Avoid falling back silently to the physical monitor when virtual-display
  setup fails.
- Report missing host dependencies and diagnostics clearly.
- Keep Gamescope optional. Gamescope is useful for a SteamOS-like session, but
  Hermes should only use it when enabled by app configuration, settings, or an
  explicit Hestia request.

## Hestia protocol support

Hermes exposes Hestia protocol v1 endpoints under:

```http
/api/hestia/v1
```

Important endpoints include:

- `GET /api/hestia/v1/capabilities`
- `POST /api/hestia/v1/session/prepare`
- `POST /api/hestia/v1/session/stop`
- `GET /api/hestia/v1/diagnostics`
- `GET /api/hestia/v1/clipboard`
- `POST /api/hestia/v1/clipboard`

Clients should gate enhanced behavior on the capabilities response. If the
Hestia API is unavailable, clients should continue through the normal
Moonlight/Sunshine flow.

## Clients

- **Android:** Artemis (ClassicOldSong's Moonlight fork) — the reference client.
- **Desktop:** Hestia — <https://github.com/MrOz59/Hestia>. No binary release
  yet; build from source. Hestia is the client tuned to Hermes' protocol
  extensions; generic Moonlight clients also work through the standard flow.

The web UI (`https://<host>:47990`) is branded Hermes. Set the displayed server
name under *Configuration → General → Server Name*; it defaults to the PC's
hostname.

## Virtual display behavior

Hermes tries to create and connect a virtual display for virtual-display
sessions before launching the configured app, selected by the
`virtual_display_backend` setting (`hermes_kms` or `evdi`).

### Hermes-KMS (preferred, zero-copy)

The compositor owns the virtual card and scans out the desktop; Hermes opens the
Hermes-KMS render node and pulls each frame as a DMA-BUF, which a real GPU
imports and encodes. On Linux/KDE Wayland this depends on:

- the `hermes_kms` kernel module loaded with `initial_enabled=1` (so the
  compositor adopts the `HERMES-1` output), and its card left on the active seat;
- `kscreen-doctor` (KWin) or a Wayland output-management protocol to enable the
  virtual output;
- a real GPU render node (e.g. amdgpu) for VAAPI encoding;
- a session where the Hermes process can access the user compositor environment.

#### Installing the Hermes-KMS driver

Hermes-KMS is an out-of-tree kernel module distributed via DKMS, so it rebuilds
automatically for every kernel update (the same way `evdi-dkms` works). The
source lives at <https://github.com/MrOz59/Hermes-KMS>.

**Option A — DKMS from a clone** (any distro with `dkms` and kernel headers):

```bash
git clone https://github.com/MrOz59/Hermes-KMS.git
cd Hermes-KMS
sudo make dkms-install        # registers + builds + installs via DKMS
sudo modprobe hermes_kms initial_enabled=1
```

The build auto-detects whether your kernel was built with clang (e.g. CachyOS)
or gcc, so no extra flags are needed. To load it automatically on every boot,
the repo ships drop-ins you can install:

```bash
sudo install -Dm644 packaging/modules-load.d/hermes-kms.conf /etc/modules-load.d/hermes-kms.conf
sudo install -Dm644 packaging/modprobe.d/hermes-kms.conf /etc/modprobe.d/hermes-kms.conf
```

To remove it:

```bash
sudo make dkms-uninstall
```

**Option B — Arch/CachyOS package** (builds and installs via DKMS, with boot
auto-load drop-ins included):

```bash
git clone https://github.com/MrOz59/Hermes-KMS.git
cd Hermes-KMS
makepkg -si
```

Verify the module is loaded and a Hermes render node exists:

```bash
lsmod | grep hermes_kms
ls /dev/dri/by-path/ | grep hermes   # expect platform-hermes-kms-render -> ../renderD*
```

Do **not** install the driver's development seat-ignore udev rule for normal
streaming — it stops KWin/GNOME from adopting the output. That rule is only for
isolated `modetest` driver testing. See the driver repository for build internals
and the zero-copy validation tooling.

### EVDI (supported alternative)

EVDI remains a fully supported backend — set `virtual_display_backend = evdi`
to use it, or Hermes selects it automatically when Hermes-KMS is unavailable. It
depends on:

- `evdi` / `evdi-dkms`
- `libevdi`
- `kscreen-doctor`
- a session where the Hermes process can access the user compositor environment

`evdi` is an **optional** dependency, not a build requirement: it lives in the
AUR (not the official repos), so the package no longer lists it under `depends`
and `makepkg -si` builds without it. Install it yourself only if you want the
EVDI virtual-display backend:

```bash
paru -S evdi        # or: yay -S evdi
```

The Audio/Video settings tab shows a live diagnostic and step-by-step install
guide when either virtual-display driver (EVDI or Hermes-KMS) is missing.

Gamescope is not required for either virtual-display path. If installed,
Hermes exposes an optional `Gamescope Steam Session` app entry that runs Steam
Big Picture inside Gamescope on top of the virtual display.

## CachyOS/Arch package build

From the repository root:

```bash
makepkg -sf
```

If the host already has all build dependencies installed and `makepkg -s` is
blocked by local dependency metadata, a local developer build can use:

```bash
makepkg --nodeps -sf
```

Install the generated package with:

```bash
sudo pacman -U ./hermes-*.pkg.tar.zst
```

The package is named `hermes` and installs under its own paths — `/usr/bin/hermes`,
`/usr/share/hermes`, and the `hermes.service` systemd user unit — so it can be
installed **side by side with the `apollo` (AUR) and `sunshine` packages**. It
declares no `provides`/`conflicts` for them; nothing collides. Start it with:

```bash
systemctl --user enable --now hermes
```

Protocol and client compatibility is unchanged: Hermes keeps the same Artemis
protocol extensions, so existing Artemis/Hestia clients keep working.

## Notes

- **Address family / web UI reachability.** The server binds dual-stack by
  default (`address_family = both`). On distros where `localhost` resolves to
  IPv6 (`::1`) first, an IPv4-only bind makes the web UI fail intermittently
  with "Failed to fetch"; dual-stack avoids that. Override under
  *Configuration → Network → Address Family* if you need IPv4-only.
- **Isolated virtual display.** With `isolated_virtual_display_option` enabled,
  the physical monitor is turned off only once a streaming session actually
  starts (and restored when it ends), not when the virtual display is created.

## Credits

Hermes builds on work from:

- ClassicOldSong's Apollo project, which established the Apollo host direction
  and Moonlight compatibility model.
- Sgtmetalmex's Apollo-CachyOS fork, whose EVDI/KDE/Gamescope patch series
  identified and fixed several Linux virtual-display issues that are important
  for Hermes stability:
  - EVDI device-index discovery.
  - KScreen/KWin virtual-output activation.
  - avoiding DRM master conflicts on EVDI cards.
  - hotplugged EVDI capture fallback.
  - EVDI CPU-buffer capture and event pumping.
  - physical-monitor recovery safety work.
  - optional Gamescope Steam Session integration.

Reference fork:

- <https://github.com/Sgtmetalmex/Apollo-CachyOS>

## Repositories

- Hermes (this host): <https://github.com/MrOz59/Apollo-Linux>
- Hermes-KMS (virtual display driver): <https://github.com/MrOz59/Hermes-KMS>
- Hestia (desktop client): <https://github.com/MrOz59/Hestia>

Report issues for the host at
<https://github.com/MrOz59/Apollo-Linux/issues>.
