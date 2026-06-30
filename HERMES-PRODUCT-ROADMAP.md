# Hermes — Product Roadmap

A prioritized task list for turning the technical fork into a product. The unique
advantage is the Hermes-KMS zero-copy pipeline: it is the only backend that can
report real capture timing because it owns the capture path. Priority goes to
high-leverage work where the infrastructure already exists.

Legend: `[x]` done · `[~]` partially done · `[ ]` not started.

## Foundation (already in place)

- [x] Track active sessions and log client connect/disconnect (`rtsp.cpp::session_count()`).
- [x] Diagnostics endpoint exists at `/api/hestia/v1/diagnostics`.
- [x] Encoder probe tests nvenc → vaapi → software in order and logs each failure.
- [~] Pipeline metrics instrumented (`capture-metric`: zero-copy ~8us vs EVDI ~180us).
- [x] Hermes-KMS exposes `GET_METRICS` ioctl (frames, waits, exports, hotplugs).
- [x] Record why a session ended; distinguish client drop from clean stop.

## Tier 1 — high value, low friction, differentiating

### A. Real-time metrics + actionable diagnostics

- [~] Expand `/api/hestia/v1/diagnostics` (or add `/metrics`) to return live:
  - [x] real encoder in use (codec + hw/sw)
  - [x] connected client(s) / active session count
  - [x] real FPS (`pipeline.fps`)
  - [x] real stream resolution (`pipeline.width` × `pipeline.height`)
  - [x] dropped frames (`pipeline.frames_dropped`)
  - [x] capture time / capture-to-encode latency (`pipeline.capture_to_encode_ms`)
  - [x] encode time (`pipeline.encode_ms`)
  - [x] real bitrate (`pipeline.bitrate_kbps`)
- [x] Source the metrics from stream/video session counters plus Hermes-KMS
  `GET_METRICS` when the backend is `hermes_kms`. (Session counters, per-frame
  video metrics, and the Hermes-KMS `GET_METRICS` ioctl are all wired; the
  `hermes_kms` block is exposed in diagnostics when that backend is selected.)
- [x] Expose the encoder fallback as a field (`encoder.fell_back_to_software` plus
  per-encoder `attempts`) rather than only logging it.
- [x] Add a streaming-readiness preflight to diagnostics.

### B. Honest encoder reporting

- [x] After the probe, record the real result (chosen encoder and why the others
  failed) in state queryable by the UI/diagnostics.
- [x] Emit an actionable message in the log and the diagnostics response
  (e.g. "VAAPI failed (reason) -> using software; check driver/permissions").

## Tier 2 — high value, medium effort

- [x] Reconnection / multiple clients: distinguish "client dropped" from "user
  stopped" and surface it. Session-end reason is recorded and exposed in
  diagnostics: `awaiting_reconnect`, `ms_since_last_end`, `total_ended`,
  `client_lost_count`.
- [x] Consistent teardown: process/audio cleanup on disconnect. Investigated in
  depth (see `session-teardown-architecture` memory): all termination paths
  (CLIENT_LOST and CLIENT_QUIT) funnel through `stop()`/`graceful_stop()` and
  end identically — audio/video/control threads are joined, the av-ping maps are
  cleared via `recv_ping`'s fail-guard, and the last session pauses the app (for
  resume) or reverts the display. No leak found; no code change needed. The
  shared nvhttp/RTSP path stays intact, preserving Moonlight/Artemis compat.
- [x] systemd: inherit the right environment (Steam/Lutris/audio/graphical
  session). The user unit now orders after `graphical-session.target`, imports
  the session environment (`DISPLAY`/`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`/audio/
  session bus) via `ExecStartPre`, and the docs cover the manual import for
  desktops that don't export it.

## Tier 3 — important, higher effort / lower differentiation

- [ ] Simplify setup flow.
- [ ] Pairing flow.
- [ ] Appliance mode.
- [ ] Resolution / bitrate configuration.
- [ ] Web UI redesign (beyond the metrics work).

## Tier 4 — out of immediate server-side scope / depends on user network

- [ ] Security / remote access.
- [ ] NAT / external network.
- [ ] Config migration.

## Additional work landed since this roadmap was written

- [x] Detect the session/compositor environment (desktop vs Gamescope).
- [x] Route capture directly to Gamescope in a standalone Gamescope session.
- [x] Configurable Gamescope backend with a Hermes-branded launcher and `HERMES_*` env.
- [x] GNOME/Mutter virtual-display support (verify-only) with honest diagnostics.
