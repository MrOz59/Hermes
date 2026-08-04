# Changelog

All notable changes to Hermes are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Add new entries under **[Unreleased]** as you work. When you cut a release,
run `scripts/bump-version.sh <major|minor|patch>` — it moves everything under
[Unreleased] into a new dated, versioned section automatically.

## [Unreleased]

### Added
- The Web UI updater can now opt in to the rolling Hermes nightly channel with
  `notify_pre_releases` (disabled by default). Nightly checks compare both the
  semantic version and the exact build commit so rolling builds at the same
  version are detected without repeatedly notifying an up-to-date install.
- Experimental Hermes-KMS shared-desktop multi-output support. A single Hermes
  server can assign simultaneous Moonlight clients separate driver outputs,
  capture pipelines, modes, and absolute-input geometry inside the host
  compositor session. Enable it with `hermes_kms_multi_output`; it requires
  Hermes-KMS UAPI 8 or newer loaded with enough `outputs=N` ([#10]).
- Prototype independent client sessions behind
  `hermes_kms_isolated_sessions`. A single Hermes server now allocates one
  Hermes-KMS UAPI 9 DRM card, private runtime directory, compositor,
  application process tree, capture pipeline, and tagged virtual input set per
  client. App profiles select an application-only Gamescope session or a Weston
  desktop session. Independent DRM cards and virtual input devices share stable
  per-card udev/libseat names, and each compositor is assigned the matching
  packaged private seat-broker socket. Missing brokers fail the launch with an
  actionable diagnostic instead of silently falling back to the host seat. The
  feature defaults off and remains explicitly experimental while audio and real
  concurrent-client behavior are validated ([#10]).
- A disposable VM input-isolation test creates real uinput devices for two
  sessions and verifies that udev assigns both the input parent and event nodes
  to distinct `hermes-kms-1`/`hermes-kms-2` seats.

### Added
- A runtime container image under `packaging/container`, which runs Hermes on a
  headless sway session with audio, XWayland and an optional Steam Big Picture
  session. It is the practical way to run Hermes on an image-based distribution
  (Bazzite, Silverblue, SteamOS): the userspace lives in the container and only
  the Hermes-KMS kernel module has to exist on the host. Distinct from the
  root-level `docker/`, which holds Sunshine's *build* images and produces no
  runnable host. Adapted from
  [SOVLOOKUP/hermes-sunshine](https://github.com/SOVLOOKUP/hermes-sunshine),
  offered for upstreaming by its author in [#6]; the config paths now follow
  Hermes' own directory (migrating volumes created before that), the Chinese
  package mirrors default to off, and compose builds locally rather than pulling
  a published image. It serves one session only, and does not compose with
  `hermes_kms_multi_output` or `hermes_kms_isolated_sessions` — see its README.

### Changed
- Hermes-KMS diagnostics now report UAPI compatibility, driver device/output
  counts, multi-output and multi-device capability, selected output numbers,
  private seat-broker readiness, and the client assigned to each active virtual
  display.
- Exclusive virtual-display mode is intentionally ignored while experimental
  multi-output is enabled, keeping physical displays and other clients' outputs
  active.
- Remote Input is rejected while independent sessions are enabled instead of
  falling through to the shared host input devices without an unambiguous
  target seat.
- Hermes keeps its configuration in its own directory. On Linux and macOS it is
  now `~/.config/hermes` (or `$XDG_CONFIG_HOME/hermes`), holding `hermes.conf`
  and `hermes_state.json` instead of `sunshine.conf` and `sunshine_state.json`.
  Hermes previously used `~/.config/sunshine`, the same directory Apollo and
  Sunshine use, so all three shared one credentials file, one apps list, and one
  set of paired clients. On first start Hermes copies an existing
  `~/.config/sunshine` into the new location and renames those two files, so
  installs carry over untouched; the original directory is left in place for
  Apollo and Sunshine. Note that all three still bind ports 47984/47989/47990,
  so only one of them can run at a time ([#14]).

### Fixed
- Setting credentials from the command line now works when the state file is
  unreadable. `save_user_creds()` refused to write anything if it could not
  parse the existing file, so `hermes --creds` — the documented way out of a
  lost or broken Web UI password — was disabled by the exact condition that
  makes you need it, leaving no recovery short of deleting the file and losing
  every paired client. The unreadable file is now moved to
  `<state file>.unreadable` and a fresh one written, so the paired clients are
  still recoverable from the copy ([#14]).
- Activating a virtual output no longer gives up on a compositor that has not
  published its output layout yet. `configure_virtual_output()` retried a head
  that had not appeared, but treated an output-management manager that had not
  sent its first `done` event as a hard failure on the very first attempt — so a
  compositor sitting at zero outputs, such as a headless session or a container
  that has just started, lost a race against its own hotplug and reported
  "output-management returned no complete output layout". Both conditions now
  retry on the same footing. Reported by @SOVLOOKUP, who worked around it
  downstream by keeping a placeholder output alive so the layout was never empty
  ([#6]).
- The config UI no longer makes browsers prompt for a client certificate. Its
  listener also serves the Hestia API, so it requests a client certificate on
  every connection while advertising no acceptable CAs — which a browser reads
  as "any certificate will do" and answers with a selection dialog. Dismissing
  that dialog aborts the connection and the login page reports
  `Failed to fetch`. Hermes now advertises its own certificate as the only
  acceptable CA, so browsers find no match and send an empty certificate
  silently. Paired Hestia clients present their certificate regardless of what
  is advertised, so API authentication is unchanged ([#14]).
- A failed port bind now says that another host is probably already using the
  port, instead of only reporting the error. The nvhttp message also printed the
  HTTPS port twice rather than the HTTP and HTTPS pair it claimed to list.
- KDE exclusive mode now blanks every physical monitor instead of only the
  primary one. On a multi-monitor desktop the secondary screens stayed lit and
  kept showing the local session while streaming. Crash-recovery state records
  all disabled outputs with their priorities, and ending the session restores
  each of them to the priority it had before the stream ([#12]).
- Cancelling an isolated launch now signals its in-progress preparation and
  compositor wait, including the atomic handoff into the active-runtime
  registry, so a late runtime cannot appear after `/cancel` returned.
- Cancelling after HTTP launch but before the RTSP handshake now invalidates
  the pending launch, and the per-client terminate action also stops that
  client's active stream without affecting other clients.
- Re-enabled update notifications after the Hermes rebrand and pointed stable
  and nightly release checks at `MrOz59/Hermes` instead of Apollo upstream.
- Renamed the Arch/CachyOS package to `hermes-streaming` because `hermes` is an
  unrelated PAM authentication package in the AUR. The new package conflicts
  with that package and replaces only legacy Hermes streaming packages older
  than `0.5.0`; the executable, service, assets, and user configuration paths
  remain unchanged.

## [0.4.1] - 2026-07-28

### Changed
- Replaced the forked Linux libnotify/AppIndicator tray backend with the current
  upstream `tray` Qt 6 backend. CMake, CI, Arch, Debian, RPM, Homebrew, and local
  build dependencies now use Qt 6, and the tray submodule points upstream again.
- Hermes-KMS setup now recommends `initial_enabled=0`, so the virtual connector
  remains disconnected until Hermes owns it for an active stream.
- Refreshed build-compatible dependency submodules while retaining the
  deliberately pinned `nanors` and `libdisplaydevice` revisions.

### Fixed
- Packaging no longer claims Sunshine/Apollo-owned udev, module-load, desktop,
  metainfo, or icon paths, allowing Hermes to be installed alongside them
  without file conflicts ([#8]).
- KDE: turning exclusive virtual-display mode off no longer allows a stale KWin
  virtual-only layout to blank the physical monitor on a later boot or hotplug.
  Hermes reapplies the complete pre-session layout and disconnects legacy
  unowned Hermes-KMS outputs enabled at module load ([#9]).
- KDE exclusive mode now persists monitor recovery state before disabling the
  physical output, refuses the transition if that state cannot be written, and
  retains it until restoration succeeds.
- wlroots: an initially empty but complete output-manager snapshot now reaches
  the existing retry loop, allowing an asynchronously hotplugged virtual
  connector to appear instead of failing immediately ([#6]).
- Tray shutdown no longer uses the synchronous libnotify close path that could
  hang Hermes when the desktop notification service was unresponsive.
- Clean clones can fetch the pinned FFmpeg build dependencies again after the
  former upstream `dist` revision became unreachable.

[#6]: https://github.com/MrOz59/Hermes/issues/6
[#8]: https://github.com/MrOz59/Hermes/issues/8
[#9]: https://github.com/MrOz59/Hermes/issues/9
[#10]: https://github.com/MrOz59/Hermes/issues/10
[#12]: https://github.com/MrOz59/Hermes/issues/12
[#14]: https://github.com/MrOz59/Hermes/issues/14

## [0.4.0] - 2026-07-02

### Added
- Appliance-mode groundwork (dormant): an `appliance_mode` config flag (off by
  default) and a read-only `platf::appliance_readiness()` that reports whether
  the host could boot straight into a headless/Gamescope streaming session
  (Gamescope availability, virtual-display availability, autologin detection,
  session environment). Surfaced under `appliance` in the diagnostics runtime
  view. No boot/login orchestration exists yet and enabling the flag has no
  runtime effect — this only paves the way for a future activation path.

### Changed
- CI now gates every package build on the test suite (`build-*` jobs
  `needs: [test]`), so nothing is compiled, released, or published as nightly
  unless the tests pass first. Pushing a `vX.Y.Z` tag re-runs test → build →
  release to promote a nightly into a stable, freshly built release.
- Package and install paths rebranded from `apollo` to `hermes` so Hermes can
  be installed **side by side** with the `apollo` (AUR) and `sunshine` packages:
  package `hermes`, binary `/usr/bin/hermes`, assets `/usr/share/hermes`, unit
  `hermes.service`, and the `hermes-monitor-recovery` helper (with its state dir
  moved to `~/.local/state/hermes`). No `provides`/`conflicts` are declared —
  nothing collides. Artemis protocol extensions and the internal Windows service
  name are unchanged, so client compatibility is preserved.
- CI builds the Arch package straight from the `PKGBUILD` via `makepkg`, so CI
  and a local `makepkg -si` produce the identical `hermes-*.pkg.tar.zst`. Removed
  the orphan `build-pkg.sh` (stale, hardcoded to 0.1.0 and the old evdi
  dependency).

## [0.3.0] - 2026-07-01

### Added
- Hermes-KMS driver diagnostics symmetric with EVDI: a `HERMES_KMS_DIAGNOSTIC`
  probe distinguishes module-not-loaded, module-not-installed, DKMS build
  failure, UAPI-too-old, missing-capabilities, and device-node-missing, exposed
  via a new `GET /api/hermes-kms/status` endpoint and the `hermesKmsInfo` block
  in `/api/config`.
- Manual install/repair tutorial for the Hermes-KMS backend in the Audio/Video
  tab, with per-diagnostic steps and the exact DKMS commands.
- Home page now surfaces driver-not-ready warnings for the selected
  virtual-display backend (EVDI or Hermes-KMS) and points to the Audio/Video
  install guide.

### Changed
- `scripts/bump-version.sh` now also updates the PKGBUILD `pkgver` (and resets
  `pkgrel` to 1), keeping the version shown in the WebUI/logs in lockstep with
  the `VERSION` file.

### Fixed
- `makepkg -si` no longer aborts on a fresh clone: `evdi` moved from a hard
  dependency to an optional one (it is AUR-only and needed only at runtime for
  virtual displays).
- Desktop entry: corrected the icon reference (`apollo`, not `apollo.svg`) and
  the launch command (`systemctl start --user`, previously the broken `--u`).
- Application description now mentions the Hestia and Artemis clients instead of
  only Artemis.

## [0.2.0] - 2026-06-30

### Added
- Single-source version scheme: the top-level `VERSION` file drives the
  CMake project version, packaging, and the version shown in the WebUI/logs.
- `scripts/bump-version.sh` to bump the version and tag a release in one step.
- Rolling `nightly` prerelease published from `main` on every successful build.
- Diagnostics now report the live stream resolution (`pipeline.width`/`height`).
- Diagnostics consume the Hermes-KMS `GET_METRICS` ioctl and expose a
  `hermes_kms` block (frame updates, acquires, DMA-BUF exports, frame waits,
  hotplugs, output enable/disable counts, and timings) when the `hermes_kms`
  backend is selected.
- Reconnection/termination observability in the diagnostics `sessions` block:
  `awaiting_reconnect`, `ms_since_last_end`, `total_ended`, and
  `client_lost_count`.

### Changed
- `package.json` renamed from `sunshine` to `hermes` and versioned at 0.1.0.
- Debian and RPM packaging now read the version from the `VERSION` file
  instead of a hardcoded value.
- The git-fallback versioning treats `main` as a release branch (no commit
  hash suffix), matching `master`.
- The systemd user service now orders after `graphical-session.target` and
  imports the session environment (`DISPLAY`/`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`,
  audio socket, session bus), so capture and launched apps inherit what they
  need instead of failing when started at login.

### Fixed
- CI: install GBM (`libgbm-dev` / `mesa-libgbm-devel`) so the Linux builds
  compile `src/platform/linux/wayland.cpp`.
- CI: correct the Windows MinHook package name and add Node.js/npm to MSYS2.
- CI: stop the Debian job failing on a redundant self-`mv` of the `.deb`.

## [0.1.0] - 2026-06-29

### Added
- Initial versioned baseline of Hermes: an Apollo-derived Linux game-streaming
  host with low-latency virtual displays via Hermes-KMS (zero-copy DRM/KMS),
  EVDI still supported, and Hestia/Moonlight/Artemis protocol compatibility.

[Unreleased]: https://github.com/MrOz59/Hermes/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/MrOz59/Hermes/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/MrOz59/Hermes/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/MrOz59/Hermes/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/MrOz59/Hermes/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MrOz59/Hermes/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/MrOz59/Hermes/releases/tag/v0.1.0
