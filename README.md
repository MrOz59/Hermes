# Hermes

Hermes is an Apollo-derived Linux game-streaming host focused on making
Moonlight/Hestia streaming less manual and more reliable on CachyOS/Arch,
with low-latency real virtual displays through Hermes-KMS.

Hermes-KMS is a purpose-built DRM/KMS virtual display driver that streams the
compositor's scanout straight into the hardware encoder as a DMA-BUF, with no
CPU readback, so it avoids the GPU→RAM→GPU copy that EVDI does. (Measured on one
machine, KWin at 720p: ~8 us/frame of capture cost on Hermes-KMS against
~180 us/frame for EVDI's CPU copy, constant regardless of resolution.)

For now EVDI is the default virtual-display backend and Hermes-KMS is opt-in and
labeled experimental; it is expected to become the default once it leaves that
status. Many Hermes features — HDR10, multi-output, independent sessions,
streaming before login — exist only on Hermes-KMS, so using them means opting in;
see [What needs Hermes-KMS](#what-needs-hermes-kms).

Hermes keeps protocol compatibility with Sunshine, Moonlight, Artemis, and
Hestia. The normal GameStream/Sunshine flow remains the fallback path, while
Hestia can use Hermes protocol extensions when the host reports support for
them. The product is branded Hermes throughout the UI, but the protocol lineage
identifier stays `Apollo` so existing Artemis/Hestia clients keep working
unchanged.

## Compatibility and support

Hermes is developed on **CachyOS with KDE Plasma (Wayland) on AMD**, because
that is the maintainer's daily system and the only configuration that is
continuously exercised. Other distributions, compositors and GPUs are supported
as far as their code paths allow, and are improved as problems get reported —
keeping several of each verified is not something one person can do alone.

[docs/compatibility.md](docs/compatibility.md) records the current state
honestly: what has actually been tested and how, what shares a verified code
path but has never been run, what is known broken, and the limitations worth
knowing before you file a bug. It covers KDE, COSMIC, GNOME, wlroots,
Hyprland and gamescope, the AMD/Intel/NVIDIA encoder paths, and the Hermes-KMS
and EVDI backends.

**Hyprland works through its own headless output, not a virtual DRM device.**
Its aquamarine backend requires every DRM device with an output to host its own
GL renderer, which a virtual display cannot do, so Hermes-KMS and EVDI are both
unusable there. Hermes asks Hyprland for a headless output instead, which it
renders on the primary GPU — see
[docs/compatibility.md](docs/compatibility.md#hyprland--verified-through-its-own-headless-output).

**Issues and pull requests are very welcome**, including reports that something
simply works — those are what move an entry from "expected to work" to
"verified".

## Direction

With Hermes 0.6.0 and Hermes-KMS 0.4.0 released, the roadmap is being
re-evaluated in the open in [#57](https://github.com/MrOz59/Hermes/issues/57).
That issue is the place to say what matters for your setup. The proposed rule
of thumb is reliability, then compatibility, installation, capture
architecture, UX, advanced features, and experimental technology last. The
proposal currently puts first:

- Stabilizing the 0.6.x stack: a readiness test that exercises the whole
  display → capture → GPU import → encoder path (today's encoder probe encodes
  a synthetic frame and can pass while capture is broken), the HDR and
  high-bitrate reports in [#56](https://github.com/MrOz59/Hermes/issues/56),
  and better multi-GPU diagnostics.
- Making applications reliably open on the virtual display
  ([#31](https://github.com/MrOz59/Hermes/issues/31)).
- Tighter Hermes + Hermes-KMS integration: detecting the driver, checking that
  the two versions match, and easier installation where the distribution
  allows it.
- Image-based distributions: Bazzite, SteamOS, CachyOS Game Mode and Fedora
  Atomic derivatives.
- Separating runtime, capture and topology in the capture architecture, and a
  native KWin capture path.

Multi-output stays experimental until it has more real-world validation, and
independent sessions are deprioritized until hardware rendering inside an
isolated compositor is solved. Gamescope stays optional: Hermes uses it only
when an app configuration, a setting, or an explicit Hestia request asks for it.

## Clients

- **Android:** [Artemis](https://github.com/ClassicOldSong/moonlight-android)
  (ClassicOldSong's Moonlight fork) — the reference client.
- **Desktop:** [Hestia](https://github.com/MrOz59/Hestia), a Moonlight-Qt based
  client tuned to Hermes' protocol extensions. Linux is its focus: the
  [v0.1.0 release](https://github.com/MrOz59/Hestia/releases) ships an
  Arch/CachyOS package, a tarball and an AppImage (the installed binary is
  `moonlight`). Windows and macOS builds are not published or tested.
- Generic Moonlight clients also work through the standard flow.

The web UI (`https://<host>:47990`) is branded Hermes. Set the displayed server
name under *Configuration → General → Server Name*; it defaults to the PC's
hostname.

## Hestia protocol support

Hermes exposes Hestia protocol v1 endpoints under:

```http
/api/hestia/v1
```

- `GET /api/hestia/v1/capabilities` (also answered at `GET /api/hestia/v1`)
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

Everything except `capabilities` authenticates with the paired client
certificate plus a per-client permission, never with the admin password — see
[docs/api.md](docs/api.md#hestia-api-v1). Clients should gate enhanced behavior
on the capabilities response. If the Hestia API is unavailable, clients should
continue through the normal Moonlight/Sunshine flow.

## Virtual display behavior

When a session asks for a virtual display — through the application's
virtual-display setting, the client's virtual-display option, a client set to
always use one, headless mode, or a host with no active display — Hermes
creates and connects one before launching the app. The
`virtual_display_backend` setting (*Configuration → Audio/Video*) picks the
driver:

| Value | Backend |
|:------|:--------|
| `evdi` | **Default.** EVDI, the stable backend. |
| `hermes_kms` | Hermes-KMS, zero-copy. Labeled experimental for now, and the only backend for the features listed below. |
| `none` | No virtual-display device: stream an output the session already has, such as a headless compositor. `headless` is accepted as an older spelling. |

On Hyprland the setting is bypassed in favor of the compositor's own headless
output (see above).

On nightly builds, a session that asks for a virtual display fails to start
when that display cannot be set up — no working driver, a display created
without a name, or a compositor that does not activate the output — instead of
streaming a physical monitor ([#58](https://github.com/MrOz59/Hermes/pull/58)).
Sessions that do not ask for a virtual display are unaffected.

Each app's `virtual-display-layout` field in `apps.json` decides where its
virtual display sits: `auto` (follow the global "Use the Virtual Display
exclusively" setting), `extend`, `mirror`, or `exclusive`. Nightly builds add
`detached`, which on KDE enables and positions only the virtual output and
leaves the physical monitor configuration untouched.

### What needs Hermes-KMS

These work only with `virtual_display_backend = hermes_kms`; on EVDI they are
unavailable, or (for HDR) the session streams SDR:

- **Zero-copy capture** on AMD and Intel, without the per-frame CPU copy EVDI
  makes.
- **HDR10** on a virtual display. EVDI does not support HDR.
- **Multi-output** (`hermes_kms_multi_output`): a separate output and capture
  pipeline per simultaneous client.
- **Independent sessions** (`hermes_kms_isolated_sessions`).
- **Streaming before login** (`hermes --prelogin`): Hermes-KMS is the only
  backend it has been run with, and its readiness report flags any other.
- **Capture-side diagnostics**: the driver's frame, wait and export counters in
  `/api/hestia/v1/diagnostics`.
- **KMS capture without `CAP_SYS_ADMIN`**, through the driver's render node.

### Hermes-KMS (experimental, opt-in)

Enable it with `virtual_display_backend = hermes_kms`. The compositor owns the
virtual card and scans out the desktop; Hermes opens the Hermes-KMS render node
read-only and pulls each frame as a DMA-BUF:

- **AMD and Intel** import that buffer and encode it through VAAPI with no copy.
- **NVIDIA** cannot: its EGL import of these system-memory buffers reads the
  wrong pages, so NVENC sessions take one CPU copy of the scanout per frame.
  Experimental work on a faster NVIDIA path is in
  [#66](https://github.com/MrOz59/Hermes/pull/66).

The read-only open is intentional: with the driver's current access policy,
Chromium/Electron's `O_RDWR` GPU probe skips this capture-only node instead of
picking it as the system renderer
([#63](https://github.com/MrOz59/Hermes/issues/63)). On Linux this backend
depends on:

- the `hermes_kms` kernel module, v0.4.0 or newer, loaded with
  `initial_enabled=0` so its connector stays disconnected until Hermes owns it
  for a stream, and with its card left on the active seat. Hermes needs UAPI 11
  or newer and refuses an older device with `UAPI <n> is too old` in the log;
- a way to enable the virtual output: `kscreen-doctor` on KWin,
  `wlr-output-management` on the wlroots family, or Mutter's DisplayConfig on
  GNOME;
- a real GPU to encode on;
- a session where the Hermes process can access the user compositor environment.

#### Installing the Hermes-KMS driver

Hermes-KMS is an out-of-tree kernel module distributed via DKMS, so it rebuilds
automatically for every kernel update (the same way `evdi-dkms` works). The
source lives at <https://github.com/MrOz59/Hermes-KMS>.

> **Match the driver to your Hermes build.**
>
> | Hermes | Hermes-KMS |
> |:-------|:-----------|
> | 0.6.0 (latest release) | the `v0.4.0` tag |
> | nightly | `main` (recommended) or `v0.4.0` |
>
> The driver's `main` grants the render node read-only access, which is what
> keeps Chromium, Discord and other Electron apps from freezing when Hermes-KMS
> registers before the real GPU. Hermes 0.6.0 still opens the node read-write,
> so that policy refuses it with `EACCES`: stay on `v0.4.0` until you move to a
> nightly or the next release. Both driver versions provide UAPI 14, which
> HDR10 needs.

**Option A — DKMS from a clone** (any distro with `dkms` and kernel headers):

```bash
git clone https://github.com/MrOz59/Hermes-KMS.git
cd Hermes-KMS
git checkout v0.4.0      # Hermes 0.6.0 only; stay on main for a nightly
sudo make dkms-install
```

`dkms-install` registers, builds and installs the module through DKMS, installs
the default module options (`initial_enabled=0 outputs=1`), the boot autoload
entry and the udev access policy under `/usr/lib`, and loads the module. If an
older Hermes-KMS is already loaded, it asks for one reboot instead of unloading
a card the compositor may hold. The build auto-detects whether your kernel was
built with clang (e.g. CachyOS) or gcc, so no extra flags are needed.

To remove it:

```bash
sudo make dkms-uninstall
```

**Option B — Arch/CachyOS package** (DKMS, with the same options, autoload
entry and udev policy):

```bash
git clone https://github.com/MrOz59/Hermes-KMS.git
cd Hermes-KMS
makepkg -si
```

This is a `-git` package: it always builds the driver's latest `main`, whatever
is checked out, so it pairs with a Hermes nightly, not with 0.6.0. It installs
`hermes-kms-dkms-git` and the optional `hermes-kms-seatd-git` private-seat
broker.

A file of the same name in `/etc/modprobe.d` overrides the packaged
`/usr/lib/modprobe.d/hermes-kms.conf`. Older Hermes-KMS packages wrote
`initial_enabled=1` to `/etc/modprobe.d/hermes-kms.conf`; change that option to
`0` before rebooting. Hermes also disconnects this legacy unowned output at
startup so an upgrade can recover without requiring the old package to be
removed first.

Verify the module is loaded and a Hermes render node exists:

```bash
lsmod | grep hermes_kms
ls /dev/dri/by-path/ | grep hermes   # expect platform-hermes-kms-render -> ../renderD*
```

Do **not** install the driver's development seat-ignore udev rule for normal
streaming — it stops KWin/GNOME from adopting the output. That rule is only for
isolated `modetest` driver testing. See the driver repository for build internals,
image-based installs and the zero-copy validation tooling.

#### HDR10

HDR10 capture on a Hermes-KMS display landed in 0.6.0 and is still under
validation. It needs the driver to advertise HDR, which is off by default and
can only be set at load time:

```bash
printf '%s\n' 'options hermes_kms hdr_enable=1 color_depth=10' | sudo tee /etc/modprobe.d/hermes-kms-hdr.conf
```

Reboot, enable HDR for the virtual output in KDE, and enable HDR in the client.
On AMD the frame stays on the GPU through VAAPI; on NVIDIA the CPU-copy path
converts it to P010 in CUDA for NVENC. A client that asks for HDR on an output
that cannot provide it streams SDR with a warning instead of being refused.
Details are in [docs/hermes_kms_hdr.md](docs/hermes_kms_hdr.md).

#### Experimental: multi-output and independent sessions

Both experiments default to off; the single-output path remains the default.

- `hermes_kms_multi_output = true` gives simultaneous clients separate outputs
  and capture pipelines, but those outputs still belong to the same host
  desktop/compositor session. The packaged module options create one output, so
  override them with enough outputs and reboot:

```bash
printf '%s\n' 'options hermes_kms initial_enabled=0 outputs=2' | sudo tee /etc/modprobe.d/hermes-kms.conf
```

- `hermes_kms_isolated_sessions = true` is the independent-session prototype.
  **It is under re-evaluation and is not recommended.** It will change in ways
  that are not backwards compatible, so a setup built on it now is likely to
  need rebuilding, and several things are known to be unfinished: a session
  composites in software rather than on the GPU, nothing bounds what a session
  may consume, a session given a Unix account of its own still hears the host's
  audio, and Remote Input is disabled. A full desktop and simultaneous real
  clients have not been validated. What follows describes what is built, not a
  feature that is ready to use.

  One Hermes server starts a separate compositor, application process tree,
  capture path, and tagged virtual input set for each client, each on its own
  Hermes-KMS card and private DRM seat. Application profiles run directly in a
  DRM Gamescope session; desktop profiles run Weston with its desktop shell and
  panel, or another compositor named by `hermes_kms_session_compositor`. It
  needs `gamescope`, `weston`, and the driver's optional private-seat broker
  (`hermes-kms-seatd-git`, or `sudo make INSTALL_BROKER=1 dkms-install` from a
  clone), configured for the user Hermes runs as:

```bash
sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto
```

That configures one seat0 host card plus private session cards (four by
default; `--session-devices N` for up to eight) and runs one `seatd` broker per
card at `/run/hermes-kms-seatd/N/seatd.sock`, asking for one reboot when a
different module topology is already loaded. Joining the `seat` group and
enabling `hermes-kms-seatd@N.service` by hand, as older instructions said, is no
longer needed — the helper removes that enablement. Hermes rejects an isolated
launch with an actionable log message when a card's broker is not available.
The brokers are experimental process/session plumbing, not a security boundary
for mutually untrusted local users. If both experimental flags are present,
`hermes_kms_isolated_sessions` takes precedence and shared-desktop multi-output
management stays inactive. The input-only/Remote Input application is
unavailable in this mode, because it has no compositor session that identifies
which private seat should receive its events.

The full reference, including the optional session and card brokers, is
[`hermes_kms_isolated_sessions`](docs/configuration.md#hermes_kms_isolated_sessions)
in the configuration docs. `scripts/vm-isolated-input-test.sh` uses a disposable
virtme-ng guest to check that the packaged udev rules assign each session's
virtual keyboards to the right private seat without modifying host rules.

### EVDI (default)

EVDI is the default and stable backend, set with
`virtual_display_backend = evdi`. Hermes does not switch to it on its own when
Hermes-KMS is selected but unavailable. It depends on:

- `evdi` / `evdi-dkms`
- `libevdi`
- `kscreen-doctor`
- a session where the Hermes process can access the user compositor environment

`evdi` is an **optional** dependency, not a build requirement: it lives in the
AUR (not the official repos), so the package no longer lists it under `depends`
and `makepkg -si` builds without it. Install it yourself if you want the EVDI
virtual-display backend:

```bash
paru -S evdi        # or: yay -S evdi
```

The Audio/Video settings tab shows a live diagnostic and step-by-step install
guide when either virtual-display driver (EVDI or Hermes-KMS) is missing.

Gamescope is not required for either virtual-display path. If installed,
Hermes exposes an optional `Gamescope Steam Session` app entry that runs Steam
Big Picture inside Gamescope on top of the virtual display.

## Installing

The quickest way is the installer, run as the user Hermes will stream for:

```bash
curl -fsSL https://raw.githubusercontent.com/MrOz59/Hermes/main/scripts/install.sh | bash
```

It installs the Hermes package for your distribution (Arch, CachyOS, Ubuntu
24.04/26.04 or Fedora 43/44) and then asks, one at a time, about the
[Hermes-KMS](#installing-the-hermes-kms-driver) driver, experimental isolated
sessions (with Hermes' own Gamescope build) and streaming the login screen.
Add `--nightly` for the rolling build of `main`, `--dry-run` to see the
commands without running them. To install by hand instead:

Every push to `main` refreshes the rolling **nightly** prerelease, and a tagged
version publishes a normal release, both at
<https://github.com/MrOz59/Hermes/releases>. CI builds these Linux packages:

| Distribution | Asset | Installs |
|:-------------|:------|:---------|
| Arch / CachyOS | `hermes-streaming-<version>-1-x86_64.pkg.tar.zst` | `sudo pacman -U ./hermes-streaming-*.pkg.tar.zst` |
| Ubuntu 24.04, 26.04 | `hermes_<version>_ubuntu<release>_amd64.deb` | `sudo apt install ./hermes_<version>_ubuntu<release>_amd64.deb` |
| Fedora 43, 44 | `hermes-<version>-1.fc<release>.x86_64.rpm` | `sudo dnf install ./hermes-<version>-1.fc<release>.x86_64.rpm` |

Match the release in the filename to the one you run: a `.deb` or `.rpm` is
built inside that release's own container, so its dependencies are the sonames
that release ships and it will refuse to install on another. Arch and CachyOS
are the configuration Hermes is developed on; the others are built in CI and not
exercised as a desktop — see [docs/compatibility.md](docs/compatibility.md).
No AppImage, Flatpak or Windows/macOS package is published.

Every package installs the binary as `/usr/bin/hermes` and a `hermes.service`
systemd **user** unit:

```bash
systemctl --user enable --now hermes
```

The nightly packages are rebuilt on every push and may be unstable. There is no
repository to subscribe to yet, so upgrading means downloading the newer asset.
A Hermes-KMS virtual display also needs the
[Hermes-KMS](https://github.com/MrOz59/Hermes-KMS) kernel module, which is
installed separately and has to match the Hermes build — see
[Installing the Hermes-KMS driver](#installing-the-hermes-kms-driver) above.

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

Install the generated package and start it with:

```bash
sudo pacman -U ./hermes-streaming-*.pkg.tar.zst
systemctl --user enable --now hermes
```

The Arch package is named `hermes-streaming` because the `hermes` name belongs
to an unrelated PAM authentication project in the AUR. Upgrading from an older
Hermes package replaces only the old `hermes` package versions below `0.5.0`.
The application paths remain `/usr/bin/hermes`, `/usr/share/hermes`, and the
`hermes.service` systemd user unit, so it can still be installed **side by side
with the `apollo` (AUR) and `sunshine` packages**.

When migrating from an older package named `hermes`, pacman will ask to remove
that conflicting package as part of the transaction; accept the removal. User
configuration is not package-owned and remains in place — which also means
uninstalling Hermes does not clear your Web UI password. Do not install the
unrelated `hermes` package offered by AUR helpers.

Installed side by side means installed, not running side by side: Hermes,
Apollo, and Sunshine all bind ports 47984/47989/47990, so start only one of
them at a time. Whichever starts second fails to bind and exits, which shows up
in the browser as `Failed to fetch` on the login page.

Protocol and client compatibility is unchanged: Hermes keeps the same Artemis
protocol extensions, so existing Artemis/Hestia clients keep working.

## Container / image-based distributions (Bazzite, Silverblue, SteamOS)

Image-based distributions keep `/usr` read-only, so there is no point at which
a package lands on the installed system and the normal install does not apply.
`packaging/container` holds a runtime image that runs Hermes on a headless sway
session with audio, XWayland and an optional Steam Big Picture session, so only
the kernel module has to exist on the host:

```bash
cd packaging/container
docker compose build
docker compose up -d
```

The virtual display still comes from the
[Hermes-KMS](https://github.com/MrOz59/Hermes-KMS) module on the host. For an
image-based host, build it into the image with that repository's
`packaging/bazzite/Containerfile` — DKMS cannot work there. Without the module
the container falls back to the wlroots headless (software) backend
(`virtual_display_backend = none`) and gives up the zero-copy path.

This image serves **one** session, so it does not compose with
`hermes_kms_multi_output` or `hermes_kms_isolated_sessions`; several clients
means one container each. See `packaging/container/README.md`.

Adapted from [SOVLOOKUP/hermes-sunshine](https://github.com/SOVLOOKUP/hermes-sunshine),
contributed upstream by its author.

Note that `docker/` at the repository root is unrelated: those are Sunshine's
*build* images and produce no runnable host.

## Notes

- **Configuration location.** Hermes stores everything in `~/.config/hermes`
  (or `$XDG_CONFIG_HOME/hermes`): `hermes.conf`, `hermes_state.json` (Web UI
  credentials and paired clients), `apps.json`, and `hermes.log`. Releases
  before 0.5.0 used `~/.config/sunshine`, shared with Apollo and Sunshine; on
  first start Hermes copies that directory across and leaves the original in
  place for them. To reset a forgotten Web UI password, stop the service and run
  `hermes --creds <username> <password>` — this keeps your paired clients,
  whereas deleting `hermes_state.json` unpairs them.
- **SteamOS / CachyOS Game Mode.** `hermes-gamemode` is a controller-friendly
  console, added to Steam as a non-Steam shortcut, for the things a machine
  that boots straight into Game Mode otherwise cannot reach: the PIN for a new
  device and a restart. See
  [SteamOS and Game Mode](docs/getting_started.md#steamos-and-game-mode).
- **Streaming before login (experimental, nightly).** `hermes --prelogin enable`
  starts Hermes with the user manager, so a host that boots with no monitor and
  no autologin can stream its login screen and carry on into the desktop.
  `hermes --prelogin status` lists what is still in the way. It has been run on
  KDE Plasma with SDDM and Hermes-KMS only; read the limits in
  [Getting Started](docs/getting_started.md#before-anyone-logs-in-experimental)
  before relying on it for a machine you cannot otherwise reach.
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
- SOVLOOKUP's hermes-sunshine, the container image in `packaging/container` that
  runs Hermes on a headless sway session, contributed upstream by its author.
  The same work identified a race in Hermes' own virtual-output activation,
  which is now fixed here.

Reference forks:

- <https://github.com/Sgtmetalmex/Apollo-CachyOS>
- <https://github.com/SOVLOOKUP/hermes-sunshine>

## Repositories

- Hermes (this host): <https://github.com/MrOz59/Hermes>
- Hermes-KMS (virtual display driver): <https://github.com/MrOz59/Hermes-KMS>
- Hestia (desktop client): <https://github.com/MrOz59/Hestia>

Report issues for the host at
<https://github.com/MrOz59/Hermes/issues/new/choose>. The forms ask for the
three things a report needs to be actionable: the Hermes version, one
copy-paste block of host information, and a debug-level log. See
[.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) for what each form is for.
