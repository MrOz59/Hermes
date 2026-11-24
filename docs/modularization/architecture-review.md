# Where to draw the first boundaries in Hermes

**Research snapshot · 21 November 2026.** This review follows the code at [workstream commit c123041](https://github.com/MrOz59/Hermes/tree/c1230419254ae191c9f95e03b031dc420742b678). Phase 0 selected session routing. The function still lives in `process.cpp`, and the tests use the process-facing API. The [original inventory](baseline.md) remains the historical record of the initial checkout. The [validation plan](validation-plan.md) adds the subsequent CI result.

## The first extraction has a real integration hazard

The release line already knows about the `primary` virtual-display layout. A future extraction based exclusively on the workstream's routing table could change which session opens for an existing configuration when brought back to `main`. The branches share base `50f2f4d`. At the pinned comparison, `main` had 105 commits unique to its side and the workstream had five. Both the route implementation and the build workflow are among the files that changed on the release side. [Pinned comparison](https://github.com/MrOz59/Hermes/compare/c1230419254ae191c9f95e03b031dc420742b678...e1cbddf53a55578d5d750541219782fa0b29838f)

| Example input to `resolve_session_route` | Workstream `c123041` | Main `e1cbddf` |
| --- | --- | --- |
| Isolation enabled, host-session flag clear, virtual display requested, default app method, layout `primary`, global `gamescope`, command `game` | Isolated application | Shared host |
| Same inputs, empty command | Isolated desktop | Shared host |
| Same inputs, explicit app method `kwin_virtual` | Isolated desktop | Isolated desktop |

These results follow from the order of conditions in the two functions. The existing `GlobalMethodLeavesHostLayoutsOnTheHost` test enumerates `extend`, `mirror` and `exclusive` on **both** revisions. Its name would suggest broader coverage than the table supplies. Before implementing on the workstream, record the differing expectation. Before integrating into the release line, characterize `primary` on the intended base, including an explicit method override. This preserves the release behavior without folding a feature port into the extraction. [Workstream implementation](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/src/process.cpp#L129-L160) · [Release implementation](https://github.com/MrOz59/Hermes/blob/e1cbddf53a55578d5d750541219782fa0b29838f/src/process.cpp#L131-L161) · [Release test](https://github.com/MrOz59/Hermes/blob/e1cbddf53a55578d5d750541219782fa0b29838f/tests/unit/test_process_session_route.cpp#L150-L162)

## What the routing component would own

Routing chooses one of three existing outcomes from values already available to the caller. Its inputs are isolation availability, the app's host-session preference, the virtual-display request, the app and global method strings, the layout string, and whether the command is empty. It can borrow strings for the duration of the call. It has no reason to retain an app, read global configuration, allocate a display, acquire a lock, or know the wire format. Keep the existing `proc::resolve_session_route` entry point as the adapter while callers migrate. Its default global method is `gamescope`. [Declaration](https://github.com/MrOz59/Hermes/blob/main/src/process.h) · [Implementation](https://github.com/MrOz59/Hermes/blob/main/src/process.cpp) · [Current characterization tests](https://github.com/MrOz59/Hermes/blob/main/tests/unit/test_process_session_route.cpp)

| Consumer | What it knows at decision time | Constraint on the extraction |
| --- | --- | --- |
| HTTPS permission check | Hestia's one-shot preparation is still pending. The caller assumes a virtual display is possible. | Keep the provisional decision at this point. Caching it as the definitive route changes inputs and can pick the wrong app identity for the permission check. |
| HTTPS launch | After Hestia preparation, `launch_requests_virtual_display` combines headless mode, the launch flag and the app flag. | Evaluate again with the actual request. Keep one-shot consumption in HTTPS and pass values to the policy. |
| `choose_isolated_profile` in the process layer | An isolated profile is already being selected. The caller passes both capability and display-request flags as true. | Preserve the caller's mapping to `desktop` or `application` and the global method it supplies. |

The two HTTPS calls are intentionally different. Moving them into one call earlier in launch would be a behavior change even if every row of the pure function's truth table still passed. [Permission check](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/src/nvhttp.cpp#L1366-L1384) · [Prepared launch](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/src/nvhttp.cpp#L1453-L1488) · [Process profile caller](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/src/process.cpp#L880-L885)

## Where ownership changes hands

| State or resource | Current owner and handoff | Regression a broader move could introduce |
| --- | --- | --- |
| Pending launch and display | The launch records cleanup responsibility. RTSP serializes cancellation against the final ANNOUNCE/start sequence using `lifecycle_mutex` and `cancelled`. Pending cleanup distinguishes an isolated runtime from a session-scoped display. | A late ANNOUNCE can start a cancelled session, or cancellation and teardown can both release the output. [RTSP launch state](https://github.com/MrOz59/Hermes/blob/main/src/rtsp.h) · [Cancellation and handoff](https://github.com/MrOz59/Hermes/blob/main/src/rtsp.cpp) |
| Active stream and isolated runtime | `stream::session::alloc` transfers display identity and clears pending cleanup. Stream destruction disconnects the isolated runtime or removes a session-scoped output. The runtime registry can retain a disconnected runtime for resume. Termination removes it under the registry lock and calls the potentially slow stop routine after unlocking. | Treating every disconnect as termination destroys resumable sessions. Moving teardown under the registry lock changes concurrency. [Stream allocation/destruction](https://github.com/MrOz59/Hermes/blob/main/src/stream.cpp) · [Runtime registry](https://github.com/MrOz59/Hermes/blob/main/src/process.cpp) |
| Display selection and encoder probing | Process setup can trigger encoder probing. Video capture reads the process display name and can write it during display recovery. A session-scoped stream carries its own display identity. | Concurrent streams can select another session's output. Inspect reader, writer and scope before moving ownership. [Video coordination](https://github.com/MrOz59/Hermes/blob/main/src/video.cpp) · [Process setup](https://github.com/MrOz59/Hermes/blob/main/src/process.cpp) |
| Hermes-KMS frame | A frame supplies plane FDs, strides, offsets, modifier and a sync-file FD. On the GPU descriptor path `kmsgrab.cpp` waits on the supplied fence, transfers plane FDs into the image and clears the source slots. There is also a CPU-mapping path. | Double-close, stale frames, premature recycling, or accidental CPU fallback can survive a successful encode test. [Frame contract](https://github.com/MrOz59/Hermes/blob/main/src/platform/linux/virtual_display.h) · [Descriptor transfer](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/src/platform/linux/kmsgrab.cpp#L2437-L2504) |
| Input, audio and platform integration | Input consumes per-session touch geometry while gamepad allocation also uses shared state. Display recovery can hold an audio context across retries. The KMS card broker and the session broker are separate executables. | Wrong-seat input, restoring the host sink too early, or moving privileged work into the host. [Input](https://github.com/MrOz59/Hermes/blob/main/src/input.cpp) · [Display recovery](https://github.com/MrOz59/Hermes/blob/main/src/display_device.cpp) · [Linux targets](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/cmake/targets/linux.cmake) |
| Configuration and protocol | Parsing, persistence and migration sit outside route selection. HTTP, RTSP and Hestia preparation feed the route through their existing contracts. | Normalizing unknown strings too early or changing defaults alters old app configurations. Keep migration fixtures and wire-visible results on the caller side. [Config parser](https://github.com/MrOz59/Hermes/blob/main/src/config.cpp) · [HTTP launch](https://github.com/MrOz59/Hermes/blob/main/src/nvhttp.cpp) · [Migration test](https://github.com/MrOz59/Hermes/blob/main/tests/unit/test_process_session_route.cpp) |

### What the include graph adds

A static count at `c123041` found **185** source/header files under `src/` and **580** distinct directed, quoted-include edges between them. Each file pair counts once. The scan considered `.c`, `.cpp`, `.cu`, `.m`, `.mm`, `.h` and `.hpp`, and resolved literal quoted names against the including directory, repository root, then `src/`. Conditional branches were included. Generated headers, third-party headers, tests, angle-bracket includes and transitive edges were excluded. These are indicators of how widely a header is used. Compiler traces are needed to estimate its build cost.

| Header | Distinct direct includers under `src/` |
| --- | ---: |
| `platform/common.h` | 49 |
| `config.h` | 38 |
| `video.h` | 24 |
| `globals.h` | 14 |
| `rtsp.h` | 10 |
| `process.h` | 9 |

`process.h` pulls in Boost.Process, JSON, platform declarations and RTSP. A policy test that can compile independently of that header is a specific architectural gain even if total build time barely changes. Conversely, converting `platform/common.h` first would touch many consumers before the team understood which types need to cross the boundary. [Process includes](https://github.com/MrOz59/Hermes/blob/main/src/process.h) · [Platform interface](https://github.com/MrOz59/Hermes/blob/main/src/platform/common.h)

## Sequence after routing

| Order | Candidate | Evidence needed before moving it |
| --- | --- | --- |
| 1 | Routing policy and its existing adapter | Exercise precedence, unknown methods/layouts and the `primary` divergence. Run the real production implementation in the focused test as well as the adapter in the host suite. |
| 2 | `video::choose_capture_readiness_mode`, if independent testing justifies the move | Existing tests cover SDR, HEVC/AV1 HDR preference, and refusal when a safe mode is unavailable. The four-boolean decision is separable from the device probe, mutex-protected result and HTTP diagnostics. Keep those consumers intact. [Decision and caller](https://github.com/MrOz59/Hermes/blob/main/src/video.cpp) · [Readiness tests](https://github.com/MrOz59/Hermes/blob/main/tests/unit/test_video.cpp) |
| 3 | Pending-to-active cleanup, once failure paths can be observed | Characterize cancellation before and during ANNOUNCE, start failure after allocation, reconnect, and two simultaneous sessions. Observe exactly which owner releases which display or runtime. |
| Later | Capture ownership, encoder adapters and compositor-specific display recovery | Record backend, GPU, compositor, fence outcomes, copy path, FD counts and concurrent teardown before changing a lifetime. Treat AMD/Intel VAAPI, NVIDIA NVENC, software fallback, Hermes-KMS and EVDI as separate cases. [Compatibility reports](../compatibility.md) |

Each row is a potential review unit. The first two can be evaluated with ordinary C++ headers and translation units. The [build-system review](build-system-review.md) addresses the separate question of how those units should eventually be compiled.
