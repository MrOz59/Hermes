# Hermes — Product Roadmap

Status: **active** · Last updated: 2026-07-30

A prioritized task list for turning the technical fork into a product. The unique
advantage is the Hermes-KMS zero-copy pipeline: it is the only backend that can
report real capture timing because it owns the capture path. Priority goes to
high-leverage work where the infrastructure already exists. This product list
complements the staged transport migration summarized in the main README; it
does not imply that HDT is a near-term deliverable.

Legend: `[x]` done · `[~]` partially done · `[ ]` not started.

## Foundation (already in place)

- [x] Track active sessions and log client connect/disconnect (`rtsp.cpp::session_count()`).
- [x] Diagnostics endpoint exists at `/api/hestia/v1/diagnostics`.
- [x] Encoder probe tests nvenc → vaapi → software in order and logs each failure.
- [x] Pipeline metrics instrumented: capture, encode, queue, packetization, FEC,
  pacing, socket-send, wire-byte, and deadline/recovery counters are available
  through bounded per-session telemetry (`capture-metric` also distinguishes
  the Hermes-KMS and EVDI capture paths).
- [x] Hermes-KMS exposes `GET_METRICS` ioctl (frames, waits, exports, hotplugs).
- [x] Record why a session ended; distinguish client drop from clean stop.

## Compatible transport migration

- [x] H0/H1: freeze the baseline and introduce typed capture, encoder,
  transport, congestion, pacing, FEC, and telemetry boundaries without changing
  GameStream behavior.
- [~] H2: bounded, deadline-aware GameStream pacing and recovery are
  implemented. The first real paired Hestia test exposed an IDR
  burst/deadline recovery loop and a later disconnect exposed an audio-FEC
  session-lifetime race. The candidate now uses bounded frame bursts,
  serialization-aware send windows, bounded post-IDR catch-up, and queued
  media ownership during teardown. The LAN/constrained reference/candidate
  matrix must still validate the fix.
- [ ] H3: conservative adaptive bitrate using feedback already available in the
  compatible ecosystem.
- [ ] H4–H6: optional Hermes feedback extensions, ICE/connectivity, and native
  identity/pairing, each with explicit capability negotiation and fallback.
- [ ] H7: first experimental HDT implementation.

HDT remains several prerequisite phases away, has no delivery date, and must
start opt-in and disabled by default. The existing GameStream path remains the
production compatibility baseline through an extended validation period.

## Tier 1 — high value, low friction, differentiating

### A. Real-time metrics + actionable diagnostics

- [x] Expand `/api/hestia/v1/diagnostics` and the web `/api/metrics` view to return live:
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
- [~] Pairing flow. Standard PIN pairing plus the Hermes/Apollo
  OTP/passphrase path, Hestia host-code UI, per-client permissions, and
  revocation exist; consolidated first-run onboarding and the later native
  identity flow remain open.
- [~] Appliance mode. Dormant backend groundwork landed: an `appliance_mode`
  config flag (off) and a read-only `platf::appliance_readiness()` that reports
  Gamescope/virtual-display/autologin/session readiness under `appliance` in the
  diagnostics runtime view. No boot/login orchestration yet; the activation path
  (autologin → Gamescope → virtual display → hermes) is the remaining work.
- [~] Resolution / bitrate configuration. Hestia protocol v1 negotiates and
  validates requested resolution/FPS/codec/bitrate before launch, while
  simplified host-side UX and H3 runtime bitrate adaptation remain open.
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
