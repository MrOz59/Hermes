# Pipeline telemetry

Hermes exposes the current host-side video pipeline snapshot under
`runtime.pipeline`. The Hestia diagnostics endpoint returns only the collector
owned by the authenticated client's active session. The administrative metrics
endpoint returns a separate aggregate across active sessions.

Up to eight concurrent session collectors are stored in fixed slots, matching
the currently advertised multi-session capability. Slots are registered and
removed with the RTSP lifecycle, and the administrative aggregate is reset
when the first session of a new active cohort starts. This is an H0 baseline
slice; it does not mark the complete H0 benchmark matrix as finished.

## Windows and percentiles

Encode and network stages use independent one-second monotonic windows for each
session and for the administrative aggregate. Each window counts every frame
and keeps at most 512 exact latency samples, so recording remains bounded and
allocation-free. Percentiles use nearest rank.

Each latency object contains `mean_ms`, `p50_ms`, `p95_ms`, and `p99_ms`.
`sampled_frames` makes truncation visible when a window exceeds 512 frames.
`window_sequence` increments for every published encode or network window and
resets with its session collector, allowing polling tools to deduplicate data.

## Frame timeline

The encoded packet keeps the existing frame index together with these monotonic
timestamps while it crosses the host pipeline:

- `frame_timestamp`: capture time supplied by the capture backend;
- `encoded_timestamp`: encoded access unit entered the video packet queue;
- `packetized_timestamp`: the first FEC block became ready for transmission;
- `first_sent_timestamp`: the first socket batch for the frame was invoked;
- `last_sent_timestamp`: the most recent socket batch for the frame returned;
  for a complete frame this is the final batch.

Duplicate frames may not have a capture timestamp. Their capture-based latency
is reported as zero instead of inventing a capture event.

Set `HERMES_FRAME_TRACE=1` when starting Hermes to emit one structured terminal
record for each sent or failed video frame:

```text
HERMES_FRAME_TRACE {"schema":1,"frame_id":42,"capture_us":...,"encode_us":...,"packetize_us":...,"first_send_us":...,"last_send_us":...,"outcome":"sent"}
```

`frame_id` is the GameStream frame index placed on the wire. Hestia retains this
same ID through decode and presentation and emits the corresponding
`HESTIA_FRAME_TRACE` record when started with `HESTIA_FRAME_TRACE=1`. Timestamps
are monotonic in each process and must only be subtracted within that process;
host and client clocks are not assumed to share an epoch. Correlate the two
records by `frame_id`.

Tracing is disabled by default because it writes one log entry per terminal
frame. Enabling it does not change the protocol or add decrypted payload to the
network capture.

Current terminal outcomes are `sent`, `send_failed`,
`send_deadline_expired`, `packet_deadline_expired`, and `awaiting_idr`.
`send_deadline_expired` and `awaiting_idr` occur before packetization.
`packet_deadline_expired` may contain partial packetization/send timestamps
when an access unit crossed its absolute deadline after transmission began.

## Queue topology snapshot

`runtime.video_queues` exposes the bounded session-registration hand-offs
without changing their behavior:

- `capture_contexts`: contexts waiting to join asynchronous capture workers;
- `encode_session_contexts`: contexts waiting to join the synchronous
  capture/encode worker.

Each object contains `active_instances`, current `depth`, summed `capacity`,
`high_watermark`, `overflow_events`, and `dropped_elements`. Depth and capacity
are summed across live queue instances. The high watermark is the greatest
depth reached by any one instance, including retired instances. Overflow
counters are cumulative for the process and retain events from queues that
have already ended.

Queue mutations still use their existing bounded lock. Diagnostics reads use
the same queue lock briefly; no additional lock, allocation, or log formatting
was added to capture callbacks or encoder frame processing.

## Metric definitions

- `capture_to_encode`: capture to encoder invocation.
- `encode`: encoder invocation to completed access unit.
- `network.send_queue`: encoded queue entry to dequeue by the video broadcaster.
- `network.packetization`: payload shaping and GameStream/RTP packet header work.
  It excludes Reed-Solomon, encryption, pacing waits, and socket calls.
- `network.fec`: Reed-Solomon shard generation.
- `network.pacer`: time actually spent waiting for an intra-frame pacing
  deadline.
- `network.send`: time inside batched or fallback socket send calls.
- `network.capture_to_last_send`: capture to completion of the final send call.
- `network.wire_bitrate_kbps`: estimated shard bytes plus the standard UDP and
  IPv4/IPv6 headers. It includes RTP/GameStream/FEC data and the optional
  encryption prefix, but excludes link-layer and tunnel-specific headers.
  Shards are counted before each block's send loop, so a frame aborted partway
  through a fallback batch remains an upper bound rather than wire accounting.
- `network.fec_overhead_percent`: repair shards divided by original data shards.
- `frames_dropped`: cumulative compatibility total for all host-side losses
  known to this session.
- `frames_replaced_before_encode`: captured frames replaced by a newer frame
  while the session encoder still had one image pending.
- `frames_dropped_encode`: frames rejected by or producing no output from the
  encoder.
- `frames_dropped_encoded_queue`: encoded access units discarded when the
  bounded shared video queue clears a full batch. Each discarded unit is
  attributed using its own session pointer, not the session of the packet that
  triggered the overflow. Every affected session is also marked for IDR
  recovery before the replacement unit becomes visible to the broadcaster.
- `frames_dropped_send_deadline`: encoded access units whose time in the shared
  queue exceeded the session's `max_frame_queue_us` budget. These are discarded
  before packetization, FEC, encryption, pacing, or socket work.
- `frames_dropped_packet_deadline`: access units whose absolute
  `encoded_timestamp + max_frame_queue_us` deadline expired during
  packetization, FEC, pacing, or sending. Hermes stops before the next
  controllable departure, marks the reference chain for recovery, and requests
  a fresh IDR through the shared cooldown.
- `frames_dropped_recovery_wait`: dependent inter frames suppressed after a
  deadline discard or encoded-queue overflow while Hermes waits for a fresh
  IDR. Requests use a monotonic 100 ms cooldown per session. A request rejected
  by the cooldown remains eligible for retry on a later dependent frame.
- `frames_dropped_reference_superseded`: older encoded access units removed
  atomically from one session when a newer IDR is inserted at the front of the
  shared queue. Units from other sessions are not counted and keep their FIFO
  order.
- `idr_requests_accepted`: explicit client and internal recovery requests that
  passed the per-session gate and raised the existing GameStream IDR event.
- `idr_requests_rate_limited`: request attempts suppressed because the same
  session had already raised an IDR request in the preceding 100 ms. This
  counts attempts, not frames or unique cooldown windows. The mandatory IDR at
  capture startup is outside both counters.

The `congestion` block describes the client's path as the congestion controller
sees it. Unlike the frame metrics it is not windowed here: the controller
already smooths its own estimate, and it is republished whenever feedback
arrives rather than on a one-second boundary. The block is `null` until enough
feedback has accumulated for the estimate to be conclusive, which is what
distinguishes a healthy link from one nothing is known about yet.

- `congestion.adaptive`: whether an adapting controller is driving the session.
  With `adaptive_fec` off the fixed controller reports no estimate at all.
- `congestion.loss_percent`: every packet the client did not receive.
- `congestion.unrecovered_loss_percent`: only the frames the client's FEC could
  not repair. This is the signal the controller acts on; loss that FEC already
  repaired is invisible to the user and must not drive adaptation.
- `congestion.clean_frame_percent`: frames whose data shards all arrived.
- `congestion.observed_frames`, `congestion.unrecovered_frames`: sample sizes
  behind the ratios, for judging how much weight the estimate deserves.
- `congestion.fec_percent`, `congestion.key_frame_fec_percent`: protection in
  force for normal and key frames, against `congestion.configured_fec_percent`.
- `congestion.available_bitrate_kbps`: conservative estimate of what the path
  can carry, derived from unrecovered loss because the compatible feedback
  carries no per-packet acknowledgements to measure a delivery rate with. It
  never exceeds `congestion.configured_bitrate_kbps`.
  **Advisory only.** The encoder fixes its bitrate when it is configured and has
  no runtime reconfiguration path, so nothing in the pipeline consumes this
  value; it is published so the adaptation can be reviewed against real sessions
  before anything is wired to apply it. A gap in this number and the measured
  `bitrate_kbps` therefore describes the path, not the stream.

These definitions intentionally separate pacing waits from socket-call time and
must remain stable when comparing p50, p95, and p99 between builds.

## H1 session telemetry boundary

Pipeline producers now publish their local observations through the typed
`ISessionTelemetry` interface. `bounded_session_telemetry_t` is the default
thread-safe implementation and retains the same fixed registry and exact
sample windows described above. It owns the synchronization used by RTSP
lifecycle, encoder, broadcaster, and diagnostics threads.

`legacy_session_telemetry_adapter_t` is the compatibility boundary for the
current GameStream path. It converts the existing opaque session pointer into a
process-local ID and converts millisecond measurements into `std::chrono`
durations before publishing typed events. Existing `video::metrics_*` callers,
endpoint response shapes, and the GameStream wire format are unchanged.

The interface can be exercised with a fake sink without starting capture,
encoding, RTSP, or a network session. Production tests also exercise the same
interface against the bounded implementation with a deterministic monotonic
clock. A future local log, benchmark, or exporter adapter can therefore be
added without teaching the media pipeline how to format or transport telemetry.

## Hestia session telemetry parity

Hestia consumes its local client-side observations through the injectable
`ISessionTelemetry` boundary. The common vocabulary is intentionally aligned
with this host collector:

| Shared concept | Hermes source | Hestia source |
|---|---|---|
| Session lifecycle | RTSP session registration/removal | connection stage, failure, and termination events |
| One-second pipeline window | per-session encode/network collector | completed receive/decode/presentation window |
| Smoothed live view | endpoint polling window | two-window diagnostic snapshot |
| Final summary | administrative/session snapshot | aggregate published during decoder shutdown |
| Terminal frame trace | `HERMES_FRAME_TRACE` | `HESTIA_FRAME_TRACE` |
| Audio recovery | transport/FEC counters when an audio collector is added | RTP/FEC, Opus PLC, decode, and recovery-drop counters |
| Audio queue pressure | sender queue delay when an audio collector is added | receiver/playback/device delay, selected buffering profile and effective limits, high-water mark, waits, and backpressure discards |
| Audio output health | not currently observable by the host | underruns, queue failures, renderer failures, and device reinitializations |

This is a semantic contract, not a new wire message. Local overlays, translated
diagnosis text, administrative snapshots, and raw session lifecycle details are
not sent to the peer. Future HDT feedback must select an explicit bounded subset
of numeric fields, version it in the protocol repository, and negotiate it
before transmission.

Hestia's legacy GameStream audio callback contains the Opus payload but no
presentation timestamp. It can therefore publish an estimated local audio
presentation delay—the sum of the receiver queue, observable playback queue,
and output-device buffer—but it must report absolute A/V offset as unavailable.
Hermes must not treat that estimate as clock synchronization or A/V drift.
Timestamped media-clock comparison remains a future negotiated HDT capability;
this observability slice does not alter the GameStream or Hermes wire formats.

The Hestia buffering profile is client-local policy context. `Default` retains
the historical SDL and SLAudio limits; `Low latency` and `Smooth playback` are
persisted opt-ins applied to the next stream. The selected name and effective
numeric limits may be compared in local/session diagnostics, but they are not
host instructions and do not require a Hermes capability bit or wire field.
