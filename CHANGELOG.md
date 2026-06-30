# Changelog

All notable changes to Hermes are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Add new entries under **[Unreleased]** as you work. When you cut a release,
run `scripts/bump-version.sh <major|minor|patch>` — it moves everything under
[Unreleased] into a new dated, versioned section automatically.

## [Unreleased]

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

[Unreleased]: https://github.com/MrOz59/Hermes/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/MrOz59/Hermes/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/MrOz59/Hermes/releases/tag/v0.1.0
