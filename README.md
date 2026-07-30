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
- Finish real paired-client validation of the bounded GameStream queues,
  deadline-aware pacing, and recovery behavior already implemented in the H2
  migration phase, then move to conservative adaptive bitrate work.

## Transition plan: GameStream today, HDT later

Hermes and Hestia are evolving incrementally rather than being rewritten. The
production media path today is still the GameStream-compatible stack. The
Hestia protocol v1 API described below is an additive control and diagnostics
layer around that stack; it is not a replacement media transport.

The transition is intentionally staged:

1. Telemetry and modular boundaries (H0/C0 and H1/C1) were added without
   changing the wire protocol.
2. Hermes H2 improves pacing, queue bounds, packet deadlines, and recovery on
   the compatible GameStream path. Its implementation is present, but the full
   reference/candidate matrix with a real paired Hestia remains an acceptance
   requirement.
3. The next stages cover conservative bitrate adaptation, richer
   Hermes↔Hestia feedback, deadline-aware recovery, automatic connectivity, and
   native identity/pairing while preserving fallback.
4. Only after those foundations are measured and stable do the H7/C7 phases
   introduce an experimental native transport.

That planned transport is provisionally named **HDT — Hermes Datagram
Transport**. The intended design is UDP-based and aware of frames, priorities,
and presentation deadlines, with authenticated encryption, explicit pacing,
frequent feedback, adaptive recovery, and multiplexed video, audio, input,
control, and FEC. The specification, test vectors, security model, and
cross-platform benchmarks must be established before its wire format is
treated as stable.

**HDT is a long-term direction, not an upcoming release feature.** It has no
delivery date, is several prerequisite phases away, and will begin as an
opt-in experiment disabled by default. GameStream compatibility remains the
working and supported path throughout a long validation period; an HDT
negotiation failure must cleanly return to that path rather than leave a
partially initialized session. QUIC Datagrams may be evaluated even later as a
comparative backend, not assumed to replace HDT.

## Hestia protocol support

Hermes exposes Hestia protocol v1 endpoints under:

```http
/api/hestia/v1
```

Important endpoints include:

- `GET /api/hestia/v1/capabilities`
- `POST /api/hestia/v1/session/prepare`
- `POST /api/hestia/v1/session/stop`
- `GET /api/hestia/v1/display/status`
- `POST /api/hestia/v1/display/recover`
- `GET /api/hestia/v1/client/permissions`
- `GET /api/hestia/v1/diagnostics`
- `GET /api/hestia/v1/clipboard`
- `POST /api/hestia/v1/clipboard`
- `GET /api/hestia/v1/commands`
- `POST /api/hestia/v1/commands/run`

Clients should gate enhanced behavior on the capabilities response. If the
Hestia API is unavailable, clients should continue through the normal
Moonlight/Sunshine flow.

When `features.multi_user_sessions` is true, the host administrator has
explicitly enabled independent sessions and an updated Hestia client can add
`"session": {"isolation": "required"}` to `session/prepare`. Hermes then
reserves a private Hermes-KMS card, compositor, process tree, capture pipeline,
and input seat for that paired client. The returned `session_id` must be sent
back to `session/stop`; it is a lifecycle token, while the paired TLS
certificate remains the authorization boundary.
Display status is resolved through that same certificate, so each Hestia sees
only its own active session and Hermes-KMS output.

A client request never enables this host setting. When it is false, Hestia
prepares a normal shared GameStream session even if the installed Hermes binary
supports the experimental code.

## Clients

- **Android:** Artemis (ClassicOldSong's Moonlight fork) — a supported
  Apollo-compatible client.
- **Desktop:** Hestia — <https://github.com/MrOz59/Hestia>. Hestia is the
  companion desktop client and the reference client for the staged
  Hermes-native evolution described above. It is not required to use Hermes;
  generic Moonlight clients work through the standard flow.

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

- the `hermes_kms` kernel module loaded with `initial_enabled=0`, leaving its
  connector disconnected until Hermes owns it for a stream, and its card left
  on the active seat;
- `kscreen-doctor` (KWin) or a Wayland output-management protocol to enable the
  virtual output;
- a real GPU render node (e.g. amdgpu) for VAAPI encoding;
- a session where the Hermes process can access the user compositor environment.

Hermes has two distinct opt-in experiments:

- `hermes_kms_multi_output = true` gives simultaneous clients separate outputs
  and capture pipelines, but those outputs still belong to the same host
  desktop/compositor session. It requires Hermes-KMS UAPI 8 or newer, loaded
  with enough outputs:

```bash
sudo modprobe hermes_kms initial_enabled=0 outputs=2
```

- `hermes_kms_isolated_sessions = true` is the new independent-session
  prototype. One Hermes server starts a separate compositor, application
  process tree, capture path, and tagged virtual input set for each client.
  Application profiles run directly in a DRM Gamescope session; desktop
  profiles run Weston with its desktop shell and panel. It requires the
  Hermes-KMS UAPI 9 or newer with one independent DRM card per client. With
  the UAPI 10 driver package, the recommended layout is an automatic pool that
  keeps a separate host card on seat0:

```bash
sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto
```

The packaged helper selects a four-session pool, writes the persistent module
configuration, reloads the role-aware udev rules, and starts the matching
private brokers. The Audio/Video page runs the same helper through `pkexec`
after one explicit confirmation. Users no longer choose a card count, enable
N services, install seat rules separately, or join the `seat` group. If an old
topology is still loaded, the configuration is saved and the UI requests one
reboot rather than forcibly unloading an in-use DRM device.

Hermes selects `/run/hermes-kms-seatd/N/seatd.sock` from the private session
index; it will
reject an isolated launch with an actionable log message if that broker is not
available. The brokers are experimental process/session plumbing, not a
security boundary for mutually untrusted local users. Per-session audio, a
full Plasma desktop, and real concurrent Moonlight clients have not been
validated yet. Both experiments default to off; the existing single-output
path remains the default. If both experimental flags are present,
`hermes_kms_isolated_sessions` takes precedence and shared-desktop
multi-output management stays inactive.

The capabilities response mirrors this host choice:
`features.multi_user_sessions` is true only while
`hermes_kms_isolated_sessions` is enabled with the Hermes-KMS backend. The
`features.hermes_kms_isolated_sessions` object separately reports
`supported`, `enabled`, and runtime `ready` state. Hestia follows the host
policy; it does not turn isolation on by itself.

The input-only/Remote Input application is intentionally unavailable in this
mode because it has no compositor session that identifies which private seat
should receive its events.

The repository includes `scripts/vm-isolated-input-test.sh`, which uses a
disposable virtme-ng guest to create two real uinput keyboards and verify that
the packaged udev rules assign them to `hermes-kms-1` and `hermes-kms-2`
without modifying host rules.

#### Installing the Hermes-KMS driver

Hermes-KMS is an out-of-tree kernel module distributed via DKMS, so it rebuilds
automatically for every kernel update (the same way `evdi-dkms` works). The
source lives at <https://github.com/MrOz59/Hermes-KMS>.

**Option A — DKMS from a clone** (any distro with `dkms` and kernel headers):

```bash
git clone https://github.com/MrOz59/Hermes-KMS.git
cd Hermes-KMS
sudo make dkms-install        # registers + builds + installs via DKMS
sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto
```

The build auto-detects whether your kernel was built with clang (e.g. CachyOS)
or gcc, so no extra flags are needed. `dkms-install` now installs the module
load, modprobe, udev, helper, and broker files together.

To remove it:

```bash
sudo make dkms-uninstall
```

**Option B — Arch/CachyOS package** (builds and installs via DKMS, with boot
auto-load, `seatd`, role-aware seat rules, and broker units included):

```bash
git clone https://github.com/MrOz59/Hermes-KMS.git
cd Hermes-KMS
makepkg -si
```

Older Hermes-KMS packages wrote `initial_enabled=1` to
`/etc/modprobe.d/hermes-kms.conf`. Change that option to `0` before rebooting;
Hermes also disconnects this legacy unowned output at startup so an upgrade can
recover without requiring the old package to be removed first.

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
sudo pacman -U ./hermes-streaming-*.pkg.tar.zst
```

The Arch package is named `hermes-streaming` because the `hermes` name belongs
to an unrelated PAM authentication project in the AUR. Upgrading from an older
Hermes package replaces only the old `hermes` package versions below `0.5.0`.
The application paths remain `/usr/bin/hermes`, `/usr/share/hermes`, and the
`hermes.service` systemd user unit, so it can still be installed **side by side
with the `apollo` (AUR) and `sunshine` packages**.

When migrating from an older package named `hermes`, pacman will ask to remove
that conflicting package as part of the transaction; accept the removal. User
configuration is not package-owned and remains in place. Do not install the
unrelated `hermes` package offered by AUR helpers.

Start Hermes with:

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
