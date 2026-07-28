# Changelog

All notable changes to Hermes are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Add new entries under **[Unreleased]** as you work. When you cut a release,
run `scripts/bump-version.sh <major|minor|patch>` — it moves everything under
[Unreleased] into a new dated, versioned section automatically.

## [Unreleased]

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

[Unreleased]: https://github.com/MrOz59/Hermes/compare/v0.4.1...HEAD
[0.4.1]: https://github.com/MrOz59/Hermes/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/MrOz59/Hermes/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/MrOz59/Hermes/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MrOz59/Hermes/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/MrOz59/Hermes/releases/tag/v0.1.0
