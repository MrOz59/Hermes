# H0/C0 video pipeline inventory

This inventory freezes the current Hermes/Hestia thread and queue topology
before H1/C1 modularization. It covers the latency-sensitive video path, not
HTTP, discovery, input, or process-management workers.

## Hermes threads

| Function | Lifetime | Priority | Responsibility |
|---|---|---:|---|
| `stream::videoThread()` | per session | normal entry; encode path raises itself to high | Joins the selected capture mode and owns the session's encode loop. |
| `video::captureThread()` | shared, or per session for a session-scoped display | critical while capturing | Captures display images and publishes the latest image to each session encoder. Used by parallel encoders. |
| `video::captureThreadSync()` | shared | high | Runs capture and encoding on one thread for non-parallel encoder backends. |
| `stream::videoBroadcastThread()` | shared while any stream is active | high | Dequeues encoded access units, packetizes, creates FEC, encrypts, paces, and sends UDP batches. |
| `stream::audioBroadcastThread()` | shared while any stream is active | critical | Sends Opus data and repair shards on the independent audio socket. |
| `stream::recvThread()` | shared while any stream is active | normal | Receives video/audio UDP pings and binds peers to sessions. |
| `stream::controlBroadcastThread()` | shared while any stream is active | critical | Processes ENet control, loss reports, IDR/RFI requests, input, and feedback. |

Hermes selects one of two capture/encode topologies:

```text
parallel encoder:
captureThread -> latest-image event -> per-session videoThread/encode
              -> global video_packets -> videoBroadcastThread

non-parallel encoder:
captureThreadSync (capture + encode for active sessions)
              -> global video_packets -> videoBroadcastThread
```

## Hermes queues and hand-offs

| Hand-off | Bound | Full/overwrite behavior | Current observability |
|---|---:|---|---|
| `capture_ctx_queue` | 30 session contexts per capture worker | `safe::queue_t` clears the queued batch when full, then inserts the newest item | Aggregated live depth/capacity, per-instance high watermark, and process-lifetime clear/discard counters under `runtime.video_queues.capture_contexts`. |
| Per-session `images` event | one pending image | A newer capture replaces the pending image | `frames_replaced_before_encode`, attributed per session. |
| `encode_session_ctx_queue` | 30 session contexts | Clears the queued batch when full, then inserts the newest item | Live depth/capacity, lifetime high watermark, and process-lifetime clear/discard counters under `runtime.video_queues.encode_session_contexts`. |
| Global `mail::video_packets` | 32 encoded access units | IDRs atomically supersede older units from their own session and enter at the front; ordinary overflow still clears the batch and marks affected sessions for recovery; over-budget units are discarded on dequeue; explicit and recovery IDR requests share a 100 ms per-session cooldown | Per-frame encode-entry timestamp, send-queue p50/p95/p99, overflow attribution, `frames_dropped_send_deadline`, `frames_dropped_packet_deadline`, `frames_dropped_recovery_wait`, `frames_dropped_reference_superseded`, `idr_requests_accepted`, and `idr_requests_rate_limited`. |
| Intra-frame shard batches | at most 64 packets, 64 KiB, or 1 ms of the pacing target per send call | Processed inline; no persistent application queue. A batch whose scheduled departure exceeds the frame's absolute deadline is discarded; fallback datagrams are checked individually | Packetization, FEC, pacing, socket-send, wire-byte, shard, packet-deadline, and recovery metrics. |

The global encoded queue mixes active sessions, but every packet carries its
own session pointer. Both successful dequeue metrics and overflow loss counters
therefore remain session-specific even though queue ownership is shared. An
overflow also marks every affected session's reference chain as broken while
the queue lock is still held.

## Hermes H1 capture-backend boundary

Display enumeration and capture-session creation now pass through the typed
`ICaptureBackend` factory. The production
`legacy_platform_capture_backend_t` delegates to the existing
`platf::display_names()` and `platf::display()` entry points, so platform
selection across KMS, Wayland, X11, NVFBC, DXGI, and WGC remains unchanged.

H1 deliberately retains `platf::display_t` as the frame, image-pool, encode
device, HDR, and codec-capability ABI. The parallel and synchronous capture
contexts own their backend for the associated thread lifetime, while the
existing display-selection fallback, session-scoped no-fallback behavior,
reenumeration, switch-display handling, and two-attempt reset policy continue
to run in the same loops.

The boundary adds no work to the per-frame callback and does not change queue
capacity, overflow policy, thread priority, memory type, zero-copy ownership,
or wire behavior. Headless tests use fake backend and display implementations
without initializing a graphical platform. Capture remains entirely host-side,
so no corresponding Hestia code change is required.

## Hermes H1 encoder-backend boundary

Encoder-device and encoder-session creation now pass through the typed
`IEncoderBackend` factory. `encoder_t` remains the immutable description of
backend formats, codec options, capabilities, and pipeline flags, while
`platf::encode_device_t` remains the capture-to-encoder interop ABI.

The production `legacy_video_encoder_backend_t` executes the existing device
and session creation paths. This preserves pixel-format and colorspace
selection, HDR metadata, FFmpeg codec options and retry fallback, hardware
context derivation, native NVENC initialization, reference-frame handling, and
asynchronous teardown policy. The parallel and synchronous pipeline contexts
own the factory for their thread lifetimes; encoder probing uses the same
legacy adapter and retains its candidate order and software fallback.

`encode_session_t` now owns the `encode_frame()` dispatch that was previously
implemented by an RTTI check on every frame. The FFmpeg and native NVENC
sessions delegate to the same encode functions and still publish into the same
bounded packet queue, so there is no new allocation or callback per packet.
Headless tests exercise a fake device, factory, and complete session hot path
without a GPU or codec.

This boundary does not change codecs, encoded bytes, GameStream framing,
capabilities, bitrate, queueing, or transport. It is entirely host-side, so no
corresponding Hestia code change is required.

## Hermes H1 transport boundary

All outbound GameStream traffic now crosses the typed `ITransport` boundary.
One `game_stream_transport_t` is owned by the shared broadcast context and is
used concurrently by the video, audio, and control broadcaster threads. Its
results distinguish a completed send, a terminal failure, and a batch that
must be retried as individual datagrams; atomic statistics expose sent
datagrams, reliable messages, bytes, failures, and batch fallbacks.

The production adapter delegates directly to the existing `platf::send()`,
`platf::send_batch()`, and ENet reliable-send paths. Datagram views are aliases
of the existing platform scatter/gather descriptors, so the adapter neither
allocates nor copies packet data. Video retains the same batch offsets and
counts, and a false batch result still triggers individual shard sends in the
same order. Audio data and its two repair shards retain the same individual
send path.

Packetization, encryption, FEC, pacing, socket ownership, destination address,
ports, MTU, headers, and payload bytes remain outside the transport adapter and
are unchanged. Reliable control payloads are still framed and encrypted before
the adapter invokes the existing ENet channel. Headless tests inject fake
datagram and reliable-channel implementations to verify zero-copy view
identity, typed fallback semantics, and statistics without opening sockets.

Because this extraction adds no capability or wire change, Hestia's existing
`GameStreamClientTransport` and receiver paths require no corresponding code
change. Future HDT or QUIC implementations can replace the host transport
behind the same boundary in their planned roadmap phases.

### Hermes listener lifetime hardening

Linux clipboard helpers are intentionally longer-lived than the request that
starts them: `wl-copy` and `xclip` may remain alive while they own a desktop
selection. Hermes launches those helpers with an explicit handle limit, so
they inherit only redirected standard streams and cannot retain HTTP, RTSP,
ENet, video, or audio sockets after a stream ends. Clipboard reads use the
same isolation.

ENet host creation also treats a bind/allocation failure as a normal empty
result before applying socket options. The broadcast startup path can
therefore report an occupied control port and reject the session without
dereferencing a null host or terminating Hermes. A socket-collision unit test
guards this failure path.

## Hermes H1 FEC boundary

The video and audio broadcasters now depend on the typed `IFecController`
boundary instead of calling the Reed-Solomon wrapper directly.

For video, `encode_request_t` carries the current GameStream payload, block
size, requested percentage, minimum parity count, and optional encryption
prefix size. The returned `encoded_block_t` is move-only: it borrows aligned
data shards from the caller-owned frame payload and owns only padding, repair
shards, shard pointers, and prefixes. The payload must therefore remain alive
until the send batch completes, matching the previous broadcaster lifetime.

Audio retains its fixed 4+2 layout through a persistent in-place encoder
created once per broadcast thread. `legacy_reed_solomon_fec_controller_t`
installs the same eight-byte Nvidia generator matrix before the first audio FEC
group and reuses the encoder for the thread lifetime.

`IFecController` remains local host policy. This extraction does not add a
capability, change RTP/GameStream headers, or send new feedback to Hestia.
Byte-equivalence tests cover video parity, final-shard padding, enforced
minimum parity, and the audio generator matrix.

## Hermes H1 packet pacing boundary

The video broadcaster now delegates its timing decisions to the typed
`IPacketPacer` boundary. One pacer remains owned by the shared broadcaster
thread, so active sessions still share the exact legacy carry-over timeline.
Frames with multiple FEC blocks also retain one cumulative sent-packet count.

`legacy_packet_pacer_t` preserves the existing integer rate calculation
(80% of 1 Gbps expressed as whole packets per 1 ms), the pre-first-batch
checkpoint, group threshold, per-block deadline update, and duplicate-frame
timestamp source. `wait_before_batch()` returns elapsed monotonic time around
the platform sleep, including timer overshoot, so the existing `pacer_ms`
metric keeps the same meaning.

The clock and sleep backend are exposed through `IPacerTimer`; deterministic
tests use a fake timer and production adapts the existing platform
high-precision timer. At the H1 boundary this extraction did not change send
batching or add audio pacing, priority queues, packet deadlines,
expired-packet removal, or a new wire capability. H2 now builds its rate-aware
policy behind the same interface.

## Hermes H2 rate-aware media pacing

The production video broadcaster now uses `rate_limited_packet_pacer_t` rather
than the H1 baseline's fixed 800 Mbps scheduler. Each session has independent
state and reads `pacing_bitrate_bps` from its congestion controller before
every frame. The fixed GameStream controller derives that target from encoder
bitrate plus configured FEC and 10% packet/header headroom, bounded between a
defensive 1 Mbps floor and the legacy 800 Mbps ceiling. A future adaptive
controller can update the same target without replacing the pacer.

The existing platform batch cap remains 64 packets and 64 KiB, but the pacer
adds a tighter limit: one batch can represent at most 1 ms at the current
target and actual synthesized wire packet size. Consecutive batches are
scheduled with the high-resolution monotonic timer. Idle time and scheduler
oversleep reset the next departure to the actual current time, so late wakeups
cannot accumulate credit and trigger catch-up microbursts. When an access
unit's estimated wire size cannot fit the remaining packet deadline at the
average target, Hermes raises only that frame's pacing rate enough to use at
most 85% of the remaining window, capped at the legacy 800 Mbps ceiling. This
handles normal encoder size variation and prevents an unrecoverable
IDR/deadline loop without restoring the old unbounded burst. Existing
`pacer_ms` session telemetry records the effective wait.

The audio broadcaster now owns the same kind of per-session state. Ordinary
Opus packets retain their natural capture cadence. At the end of each 4+2 FEC
block, the data packet and two repair packets are spaced across one
`packetDuration` window instead of being emitted back-to-back. RTP sequence,
timestamp, encryption, Nvidia repair matrix, UDP socket, destination, and QoS
are unchanged.

Both broadcaster registries are LRU-bounded to 32 sessions, preventing session
churn from growing pacing state indefinitely while another active session
keeps the shared worker alive. Deterministic fake-clock tests cover batch
limits, exact spacing, oversleep, audio repair bursts, per-session reuse, and
registry eviction.

This increment changes only host departure timing and batch size. GameStream
packet bytes and control capabilities are unchanged, so Hestia requires no
code update. Priority scheduling and deadline-aware removal are handled by the
H2 increments below.

## Hermes H2 encoded-frame queue protection

The fixed GameStream congestion target now sets `max_frame_queue_us` to two
negotiated frame intervals, rounded up and clamped to 8–100 ms. Invalid
framerates use a defensive 50 ms fallback. A zero budget still explicitly
disables deadline enforcement for injected or future controllers.

Immediately after dequeue, the video broadcaster compares this budget with the
access unit's monotonic `encoded_timestamp`. An expired unit is discarded
before payload replacement, GameStream/RTP packetization, Reed-Solomon,
encryption, pacing, and socket calls. This bounds the useful encoded queue
without changing its capacity or clear-the-batch/keep-the-newest behavior.

The policy keeps bounded per-session recovery state. IDR units are classified
as reference priority and inter frames as normal priority. Dropping any unit
starts recovery through the existing GameStream IDR event; dependent inter
frames are then discarded without packet work until a fresh IDR arrives. The
request is raised once for an expired inter frame and is raised again only if
the replacement IDR itself has already expired.

Queue overflow feeds the same policy from the encoder side. Every session
represented in the discarded batch is marked synchronously before the newly
encoded unit becomes visible to the broadcaster. The first dependent unit
dequeued for each affected session is suppressed and raises one IDR request;
later dependents remain suppressed without repeated requests. A fresh queued
IDR can satisfy recovery immediately.

Diagnostics distinguish deadline expiry from dependent-frame suppression, and
terminal frame tracing records both outcomes. Policy state is LRU-bounded to
32 sessions, protected across the encoder and broadcaster threads, and erased
with the RTSP session lifecycle. Deterministic tests cover expiry, recovery,
budget disabling, priority classification, concurrent access, churn, and an
actual mixed-session overflow of the 32-unit production queue.

No packet or control bytes changed, so Hestia needs no matching code change.
Packet-level deadline enforcement and IDR burst protection are described in
the subsequent H2 sections.

## Hermes H2 packet-deadline enforcement

The queue budget is now also an absolute departure deadline:
`encoded_timestamp + max_frame_queue_us`. `packet_pacing_config_t` carries it
as an optional monotonic time point. A zero budget or missing encode timestamp
keeps the previous no-deadline behavior.

The video broadcaster checks the deadline before beginning another FEC block,
before every paced batch, and before every individual datagram when the
platform batch backend falls back. The pacer does not sleep when its next
scheduled departure is already beyond the deadline and detects timer
oversleep that crosses it. Departure exactly at the deadline remains valid,
matching the encoded-queue comparison.

`sendmmsg`/GSO/USO batches are indivisible once submitted, so the finest
controllable bound is the existing rate-aware batch of at most 1 ms. If a
fallback batch expires partway through, only processed RTP sequence numbers
are consumed; remaining shards and later FEC blocks are abandoned.

Any deadline abort increments `frames_dropped_packet_deadline`, emits the
`packet_deadline_expired` frame trace, marks the reference chain broken, and
requests a fresh IDR through the same 100 ms per-session gate. A suppressed
request remains pending and is retried by a later dependent frame.

This remains host-only GameStream policy: packet headers, encryption,
capabilities, destination, and client behavior do not change. Deterministic
fake-clock tests cover a scheduled departure beyond the deadline, oversleep,
the exact-boundary case, legacy-pacer behavior, recovery-cause retention, and
the new per-session metric.

## Hermes H2 explicit GameStream priorities

`media_priority_e` now gives the compatibility path one shared vocabulary:
P0 control/input/feedback, P1 audio, P2 IDR/reference video, P3 normal video,
and P5 FEC. This is policy metadata inside Hermes and does not add a GameStream
header or capability.

GameStream already separates control, audio, and video into independent
workers and sockets. Hermes therefore enforces P0/P1 with critical workers,
keeps the video worker high, and retains the existing audio-over-video socket
QoS mapping. It deliberately does not place all channels behind one mutex:
control and audio can continue while a video frame is packetized or sent.

The shared encoded-video queue is the point where P2 and P3 really contend.
When an IDR is enqueued, Hermes atomically removes older access units belonging
to that same session and inserts the IDR at the front. Other sessions retain
their relative FIFO order. This prevents an IDR from being followed by older
dependent frames and lets recovery bypass unrelated normal-video backlog.
Normal bounded-overflow recovery still runs if the queue remains full after
the session-local cleanup.

Original data shards retain their position before repair shards within each
video FEC block and audio FEC group, expressing P3-before-P5 without changing
packet bytes. `frames_dropped_reference_superseded` distinguishes intentional
IDR supersession from deadline, recovery-wait, encoder, and overflow losses.
Tests cover class order, worker mapping, generic priority insertion, mixed
session FIFO preservation, and the new counter.

Explicit GameStream IDR requests and internal queue-recovery requests pass
through the same monotonic 100 ms gate keyed by session. A rejected internal
attempt does not consume the recovery retry: dependent frames remain suppressed
and retry when the window opens. The mandatory capture-start IDR is exempt,
and accepted IDRs continue through the normal rate-aware video pacer.

A fully unified per-packet scheduler remains HDT work. On GameStream it would
serialize sockets that currently make progress independently. The remaining
H2 acceptance work is empirical execution of the LAN/netem matrix; the
compatibility-path implementation items are complete. The versioned harness
now pairs Hermes one-second windows with exact Hestia terminal-frame
percentiles, enforces the LAN regression budget, constrained-link improvement,
frame-ID loss, minimum sample counts, and a 100 ms ceiling on the worst
published host send-queue p99, and returns a failing process status when any
criterion is not met.

## Hermes H1 congestion-control boundary

Each session now owns an `ICongestionController`. The shared video broadcaster
reports one compact `sent_packet_batch_t` after each existing platform send
batch, including the frame and first RTP sequence, data/repair split, wire
size, keyframe marker, and monotonic completion time. This avoids allocation
and a virtual callback per individual packet. The video handshake also reports
the selected IPv4/IPv6 path and effective maximum datagram size.

The control thread feeds the same controller from both GameStream feedback
formats. The 32-byte legacy loss report is decoded little-endian without
unaligned pointer casts. The 21-byte big-endian `SS_FRAME_FEC_STATUS` already
emitted by Hestia's `moonlight-common-c` is now recognized instead of being
logged as an unknown control message. Runt payloads are rejected before any
field is read.

`legacy_fixed_congestion_controller_t` exposes the already selected encoder
bitrate, pacing rate, and configured FEC ratio as a typed target, but
observations intentionally cannot change it. H1 introduced this without
changing behavior; H2 now derives its fixed pacing rate from encoder and FEC
budgets as documented above. Hestia still requires no code change; adaptive
estimation and encoder-target application remain later-phase work.

## Hestia threads

| Thread | Lifetime | Responsibility |
|---|---|---|
| `VideoRecv` | stream | Receives/decrypts UDP, reorders RTP/FEC packets, and reassembles decode units. |
| `VideoDec` | conditional | Moonlight-common decoder submission thread for callback modes. Hestia's FFmpeg path advertises pull rendering, so `FFDecoder` normally pulls decode units instead. |
| `FFDecoder` | stream | Submits assembled units to FFmpeg, receives decoded frames, and attaches the GameStream frame ID. |
| `PacerVsync` | presentation-path dependent | Waits for or receives display synchronization and moves frames from pacing to render. |
| `PacerRender` | renderer dependent | Renders frames when the renderer supports a dedicated render thread; otherwise the UI/main thread renders. |

## Hestia queues

| Queue | Bound | Full/overwrite behavior | Current observability |
|---|---:|---|---|
| UDP socket receive buffer | configured for 2,048 maximum-size packets | Kernel UDP loss when exhausted | Network drops, RTT, and RTT variance; no live buffer depth. |
| Moonlight RTP reorder/FEC lists | Protocol-bounded shards for the active FEC block/frame | Retains pending shards and completed multi-FEC blocks; purges them when a frame becomes unrecoverable or a newer frame supersedes it | `moonlight-common-c` current/max/sample/total packet depth plus Hestia per-frame peak-depth p50/p95/p99/max. |
| Moonlight decode-unit queue | 15 units | Rejects the offered unit and requests recovery; only used outside direct/pull modes | Moonlight queue count API; normally bypassed by Hestia FFmpeg pull mode. |
| FFmpeg `m_FrameInfoQueue` | tied to submitted frames awaiting decoder output; no explicit `QQueue` cap | Cleared on decoder reset | Depth histogram 0/1/2/3+, decode latency, and terminal frame trace. |
| Pacer pacing queue | 3 frames | Drops oldest on hard limit or when above adaptive target | Depth histogram 0/1/2/3+, adaptive target, queue latency, and drop reason. |
| Pacer render queue | 3 frames | Drops oldest on hard limit or catches up to the renderer target | Depth histogram 0/1/2/3+, target, render latency, and drop reason. |

## Frame correlation

Hermes writes the 32-bit GameStream frame index into every video packet. The
same value becomes Hestia's `DECODE_UNIT.frameNumber` and is retained through
the decoded `AVFrame` until it is presented or discarded.

Enable `HERMES_FRAME_TRACE=1` and `HESTIA_FRAME_TRACE=1` to emit the two local
timelines. Join them by `frame_id`; do not subtract timestamps between machines
because each process uses its own monotonic clock epoch.

All application-owned queues in this inventory now expose a bounded depth or
depth distribution. Kernel socket-buffer occupancy remains platform-owned;
loss and RTT variance are the portable signals currently retained for it.
