# Hermes — Product Roadmap

A prioritized task list for turning the technical fork into a product. The unique
advantage is the Hermes-KMS zero-copy pipeline: it is the only backend that can
report real capture timing because it owns the capture path. Priority goes to
high-leverage work where the infrastructure already exists.

Legend: `[x]` works · `[~]` works with a caveat named beside it · `[ ]` not started.

**Where the work lives.** Everything under "Foundation", Tier 1 and Tier 2 is on
`main` and released. Most of what is described under "Isolation and multi-user",
"Hermes-KMS driver surface" and "Game Mode" exists **only on the local branch
`feat/hermes-kms-uapi-v13`, which has 18 commits and has never been pushed**.
The capture-protocol work below that is newer still and not yet committed. None
of it is in a release. Read every `[x]` in those sections as "written and
tested", not as "shipped".

**What "tested" means here.** Anything touching seats, accounts, polkit or DRM
master is verified in a virtme-ng guest, not on a desktop — the note beside each
item says which. Per-compositor capture is verified against the compositors named
in `docs/compatibility.md` and nowhere else.

## Foundation (already in place)

- [x] Track active sessions and log client connect/disconnect (`rtsp.cpp::session_count()`).
- [x] Diagnostics endpoint exists at `/api/hestia/v1/diagnostics`.
- [x] Encoder probe tests nvenc → vaapi → software in order and logs each failure.
- [~] Pipeline metrics instrumented (`capture-metric`: zero-copy ~8us vs EVDI ~180us).
      The numbers are from one machine; they have not been reproduced elsewhere.
- [x] Hermes-KMS exposes `GET_METRICS` ioctl (frames, waits, exports, hotplugs).
- [x] Record why a session ended; distinguish client drop from clean stop.

## Tier 1 — high value, low friction, differentiating

### A. Real-time metrics + actionable diagnostics

- [x] Expand `/api/hestia/v1/diagnostics` (or add `/metrics`) to return live:
  - [x] real encoder in use (codec + hw/sw)
  - [x] connected client(s) / active session count
  - [x] real FPS (`pipeline.fps`)
  - [x] real stream resolution (`pipeline.width` × `pipeline.height`)
  - [x] dropped frames (`pipeline.frames_dropped`)
  - [x] capture time / capture-to-encode latency (`pipeline.capture_to_encode_ms`)
  - [x] encode time (`pipeline.encode_ms`)
  - [x] real bitrate (`pipeline.bitrate_kbps`)
- [x] Source the metrics from stream/video session counters plus Hermes-KMS
  `GET_METRICS` when the backend is `hermes_kms`.
- [x] Expose the encoder fallback as a field (`encoder.fell_back_to_software` plus
  per-encoder `attempts`) rather than only logging it.
- [x] Add a streaming-readiness preflight to diagnostics.

### B. Honest encoder reporting

- [x] After the probe, record the real result (chosen encoder and why the others
  failed) in state queryable by the UI/diagnostics.
- [x] Emit an actionable message in the log and the diagnostics response.
- [ ] Make the probe prove capture, not just encode. Every encoder probe encodes a
      *synthetic* frame and never touches the capture path, so a host whose capture
      is broken probes green and streams a black picture. This is how the two worst
      field reports so far presented themselves (#29, and the GBM-device bug behind
      it). A readiness check that captures one real frame and looks at it would have
      caught both before a client ever connected.

## Tier 2 — high value, medium effort

- [x] Reconnection / multiple clients: session-end reason recorded and exposed as
  `awaiting_reconnect`, `ms_since_last_end`, `total_ended`, `client_lost_count`.
- [x] Consistent teardown: process/audio cleanup on disconnect. Investigated in
  depth (see the `session-teardown-architecture` note): all termination paths
  funnel through `stop()`/`graceful_stop()` and end identically. No leak found;
  no code change needed. The shared nvhttp/RTSP path stays intact, preserving
  Moonlight/Artemis compatibility.
- [x] systemd: inherit the right environment (Steam/Lutris/audio/graphical session).

## Isolation and multi-user *(branch only — the largest stream, absent from earlier versions of this file)*

One host giving each paired client a session of its own. This is the capability
the last stretch of work went into, and it is not represented anywhere above.

**Status: under re-evaluation, not recommended for use.** The parts below are
built and tested, but the shape of the feature is not settled and will change
incompatibly. The caveat and the two unchecked items at the end of this section
are not polish - they are the reasons it cannot be recommended yet.

- [x] A DRM card, seat, Wayland socket and input devices per session.
- [x] A Unix account per paired client, created on first use by
  `hermes-session-broker` — a socket-activated root service authorized by
  `SO_PEERCRED` against a root-owned allow file. Accounts are `hermes-sNN`,
  derived from a slot and never from anything a client sent.
- [x] The session runs as that account, in a transient systemd unit, with the
  host's session bus, audio server and credentials removed from its environment.
- [x] A polkit rule that refuses the `hermes-session` group every action. Not
  optional: a session on a local seat counts as "active and local" to polkit,
  which a stock desktop hands over a hundred passwordless actions.
- [x] An audio sink per session, so a client hears its own session and the host
  hears the host.
- [x] `GET /api/sessions/list` and `POST /api/sessions/terminate`, with a panel in
  the web UI — an isolated session outlives the stream that started it, so
  without this there was no way to see one running with nobody attached.
- [x] Verified end to end: `scripts/session-broker-test.sh`, 44 checks, all
  passing in a virtme-ng guest. It creates real accounts and asks systemd for
  real units, so it only runs there.
- [~] **The session renders in software.** This is the honest state of the
  feature and the largest thing wrong with it. weston takes KMS nodes only, for
  both its display and its rendering device, and the only KMS node backed by a
  real GPU belongs to seat0 — which a session on a private seat cannot open.
  A labwc profile was written to escape that (wlroots takes the two separately)
  and it gets further: it opens the private-seat card and accepts the render
  split. It then fails every swapchain, because wlroots allocates scanout buffers
  on the *display device's own* render node, and Hermes-KMS's is capture-only
  with no Mesa driver behind it. Closing this needs a driver change — a render
  node Mesa can drive, or none at all — not anything in Hermes.
- [ ] Nothing bounds what a session may consume. No cgroup limits, no quota on
      the account's home, no cap on concurrent sessions beyond the eight-slot pool.
- [ ] No story for removing a client's data. Accounts are deliberately never
      removed implicitly, and `PURGE` exists, but nothing in the UI reaches it.

## Hermes-KMS driver surface *(branch only)*

- [x] UAPI v13: bindings revoked before an output is disabled, per-session binding
      counters in diagnostics, session cards ordered by session index.
- [x] `hermes-kms-card-broker`: creates a card on a private DRM seat at runtime
      through configfs, so an exhausted pool is answered rather than refused.
- [x] A refused mode names the envelope the driver would have accepted instead of
      leaving `EINVAL` to be guessed at.
- [~] The driver still marks two modes preferred — its synthetic EDID timing and
      the mode the client asked for — so a compositor left to choose picks the
      larger. Every profile works around it by naming the mode. Redundant the day
      the driver clears `DRM_MODE_TYPE_PREFERRED` from the EDID modes.
- [ ] Driver release state is behind the host: the last release is v0.3.2, 0.4.0
      is unreleased, and installed modules are test builds. Anything above that
      depends on a driver users cannot yet install.

## Capture coverage per compositor *(partly uncommitted)*

"Wayland" is not one case. Which protocol a compositor speaks decides whether a
session is capturable at all.

- [x] wlroots family (Hyprland, labwc, sway) through wlr-screencopy, plus
      Hyprland headless outputs as virtual displays.
- [x] KWin and GNOME/Mutter for the *host* desktop via their own paths; see
      `docs/compatibility.md` for what is actually verified where.
- [x] Capture buffers allocated on a GPU that can produce them, and a capture that
      can never work now ends with the reason instead of retrying at frame rate.
      This was the black-screen-with-working-audio failure from #29.
- [~] `ext-image-copy-capture` implemented and verified against labwc, but it does
      **not** deliver what the #29 report claimed it would: KWin 6.7.4 does not
      implement the protocol at all — it speaks only `zkde_screencast_unstable_v1`.
      What this backend buys is GNOME and the wlroots compositors that have moved
      off screencopy. Uncommitted.
- [ ] Capturing an existing KDE desktop still has no path. It needs KWin's own
      protocol (upstream Sunshine merged a `kwingrab.cpp` for exactly this) or the
      portal.
- [ ] No SHM fallback for a compositor that offers only shared-memory screencopy;
      such a session is refused with an explanation rather than captured slowly.

## Game Mode / appliance *(branch only)*

- [x] `hermes-gamemode`, a controller-driven console added to Steam as a non-Steam
      shortcut: PIN entry for a pairing client and a restart, on a machine with no
      desktop to open the web UI on and no keyboard to type a password into.
- [x] The user unit actually starts in Game Mode, where nothing activates the XDG
      target — before this it was reported `enabled` and never ran.
- [~] Appliance mode remains a dormant flag plus a read-only readiness report
      (`appliance` in diagnostics). The activation path — autologin → Gamescope →
      virtual display → hermes — is still not orchestrated.
- [~] Pairing flow: a waiting client is now noticed and answerable without a
      desktop (`pending_pair`, `pin.html`, the console's keypad). The flow itself
      is unchanged from upstream.

## Tier 3 — important, higher effort / lower differentiation

- [~] Simplify setup flow. The Game Mode console covers the appliance case; a
      first-run experience on an ordinary desktop is untouched.
- [~] Resolution / bitrate configuration. `max_bitrate`, `fallback_mode`,
      `default_scale_factor` and a per-client `display_mode` override all exist.
      What is missing is a coherent place to reason about them.
- [ ] Web UI redesign. Panels have been added (known-issues feed, session list);
      nothing has been redesigned.

## Tier 4 — out of immediate server-side scope / depends on user network

- [ ] Security / remote access.
- [ ] NAT / external network.
- [ ] Config migration.

## Open questions this file cannot answer

- **How much longer to build on upstream.** New work lands better as owned
  modules than as edits to upstream Sunshine files, and the priorities above do
  not say where that line is. It is the most structural decision outstanding.
- **The unmerged branch.** Three of the sections above describe work that exists
  in one place, on one machine. Deciding what of it reaches `main` — and in what
  order — is a prerequisite for the roadmap meaning anything.
