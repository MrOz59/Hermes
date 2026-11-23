# Modularization baseline and dependency inventory

This inventory starts from commit `b5947fc33c78f890f065cffd5d37accf693c8c67`
on `refactor/modularization-modernization` (2026-11-16). It describes observed
source dependencies, not independently implemented modules or a proposed
directory layout. Implementation and verification in this workstream are
AI-assisted and require maintainer review before integration.

For the active Linux build and verification commands, see the
[mcpp build guide](mcpp-migration.md). The commands below describe the recorded
baseline checkout.

## Build and test baseline

The existing build has these constraints:

| Item | Source and observed value |
| --- | --- |
| Minimum CMake | `CMakeLists.txt`: 3.20 |
| Main executable | `cmake/targets/common.cmake`: target `sunshine`, C++23 |
| CUDA | Same file: C++17, with separate compiler selection in `cmake/compile_definitions/linux.cmake` |
| Main unit/integration suite | `tests/CMakeLists.txt`: `test_sunshine` recompiles the host source list without `src/main.cpp` |
| Main suite invocation | `.github/workflows/build.yml`: run the GoogleTest binary directly under Xvfb; no CTest registration for that binary |
| Existing focused example | `tests/network_lifecycle/mcpp.toml`: a focused executable for production networking code with no capture SDK requirements |
| Host CI | Arch container, CUDA disabled, required pinned PyroWave SDK; additional headless Sway layout tests |
| Package CI | Arch, Ubuntu 24.04/26.04 and Fedora 43/44; secure-loader fixtures are separate jobs |
| Other platforms | Windows build job explicitly disabled; no macOS build job in this workflow. Source presence does not establish support. |

The main workflow originally did not include this workstream in its push or
pull-request branch filters. The Actions API returned zero runs for the branch
at the initial inspection. This prerequisite change adds the workstream to
those filters so subsequent changes receive the existing host/package checks.
Release, nightly and release-candidate conditions remain unchanged.

### Local attempt before production changes

Environment: Debian GNU/Linux 13, x86-64, GCC 14.2.0, CMake 3.31.10 and
Ninja 1.11.1 (pip package 1.11.1.4). CMake and Ninja were installed into a
temporary tools directory, without changing project requirements.

The following CI configure command was attempted:

```sh
cmake -B build-modularization-baseline -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ENABLE_WAYLAND=ON \
  -DSUNSHINE_ENABLE_X11=ON \
  -DSUNSHINE_ENABLE_DRM=ON \
  -DSUNSHINE_ENABLE_CUDA=OFF \
  -DBUILD_TESTS=ON \
  -DSUNSHINE_REQUIRE_PYROWAVE=ON
```

Result: **configuration blocked by missing prerequisites**, before any
production change. Diagnostics named the uninitialized
`third-party/moonlight-common-c/enet`, `third-party/Simple-Web-Server` and
`third-party/libdisplaydevice` submodules, then stopped with:

```text
PyroWave was required but pyrowave-shared >= 0.5 was not found
```

The configured Boost 1.89.0 and nlohmann_json 3.11.3 FetchContent downloads
succeeded. The initial clean clone did not initialize submodules; only the
pinned GoogleTest submodule is needed for the first focused extraction tests.
The full prerequisite recipe remains the existing workflow's setup,
recursive checkout and `scripts/build-pyrowave-sdk.sh`.

This is an environment limitation, not evidence of a source regression.
`test_sunshine` was **not built or run locally**. There is no local full-host
build/test time or executable-size baseline. No GPU, compositor, Hermes-KMS
or EVDI device was available. Streaming latency, CPU usage, frame copies and
memory consumption were **not measured**.

## Compilation-unit inventory

At the starting commit, `src/` contains 84 implementation files
(`.c`, `.cpp`, `.cu`, `.m`, `.mm`) and 101 headers (`.h`, `.hpp`). Of the
implementation files, 24 are at the root of `src/`, 25 directly under
`src/platform/linux/`, and six under its `input/` directory. These are source
counts, not a claim that every file is compiled by every configuration.

`cmake/compile_definitions/common.cmake` explicitly assembles the portable
source list, appends the NVENC source glob and platform sources, and supplies
it to both the executable and main tests. Linux source selection in
`cmake/compile_definitions/linux.cmake` adds capture implementations according
to CUDA/DRM/VAAPI/Wayland/X11 discovery, but always includes the virtual-display
implementation and broker clients. The privileged card/session brokers are
separate executable targets in `cmake/targets/linux.cmake`; keep that boundary.

Largest relevant units, counted at the starting commit:

| File | Lines | Responsibilities currently mixed |
| --- | ---: | --- |
| `src/platform/linux/virtual_display.cpp` | 8,968 | Driver discovery, EVDI and Hermes-KMS, compositor layout, capture acquisition and monitor recovery |
| `src/process.cpp` | 5,343 | App loading/migration, routing, launch commands, isolated-runtime registry, display/audio/device setup and cleanup |
| `src/confighttp.cpp` | 4,328 | Configuration API, app/client management and diagnostics |
| `src/video.cpp` | 3,978 | Encoder discovery/probing, capture coordination, conversion, encode sessions and metrics |
| `src/platform/linux/kmsgrab.cpp` | 3,274 | DRM and Hermes-KMS capture, frame/fence handling, cursor composition and backend adaptation |
| `src/stream.cpp` | 2,630 | Session state, transport threads, packet handling, input/audio/video ownership and teardown |
| `src/nvhttp.cpp` | 2,420 | Pairing, launch permission and app lookup, Hestia preparation, launch/cancel and server state |
| `src/platform/linux/wayland.cpp` | 1,917 | Compositor/output discovery, capture protocol objects and DMA-BUF lifetime |
| `src/input.cpp` | 1,912 | Input decoding/translation, device selection, global gamepad allocation and per-session state |
| `src/config.cpp` | 1,645 | Defaults, parsing/validation and mutation of process-wide configuration |
| `src/rtsp.cpp` | 1,503 | Pending launches, negotiation, cancellation and transition into a stream |
| `src/display_device.cpp` | 876 | Configuration/recovery scheduling and display-related audio holds |

## Observed dependency edges

These edges come from includes and actual calls, not filenames. They are
logical dependencies; some are bidirectional rather than a clean layer stack.

| Consumer | Dependency and evidence | Boundary to preserve or investigate |
| --- | --- | --- |
| `main.cpp` | Initializes configuration, display recovery, platform services, process management and input; starts HTTP/config/RTSP threads | Composition root and destruction order |
| `nvhttp.cpp` | Reads `proc::ctx_t`, resolves session routing, starts app/isolated runtimes, publishes `rtsp_stream::launch_session_t` | Policy can be pure; launch and rollback remain coordinated |
| `rtsp.cpp` | Owns pending launches; calls `stream::session::alloc/start` and `proc::proc.terminate_isolated` on failed pending launches | Explicit handoff versus cancellation |
| `stream.cpp` | Holds input and broadcast resources; starts audio/video; destructor disconnects an isolated runtime or releases a session display | Stream lifetime differs from isolated-runtime lifetime |
| `process.cpp` | Calls `VDISPLAY`, encoder probing, session brokers, session audio and device isolation | Route selection must not own these resources |
| `video.cpp` | Calls `platf::display`, encoder adapters, `VDISPLAY`, and reads/writes `proc::proc.display_name` | Verified process/video back-edge; do not relocate display ownership casually |
| Linux virtual-display code | Reads `config::video`, exposes driver frame/cursor FDs, and coordinates compositor-specific layout | Separate discovery/layout/acquisition only after lifetime tests |
| Capture and encoders | Share `platf::display_t`, `platf::img_t`, encode-device contracts and Linux graphics descriptors | FD ownership and synchronization are part of the contract |
| `input.cpp` | Consumes capture geometry through touch-port events and selects shared or session-specific platform input | Keep coordinates and device isolation tied to the correct session |
| `display_device.cpp` | `DD_DATA` protects a retry scheduler; display changes can hold an audio context | Recovery work can outlive an individual API call |
| `audio.cpp` | Shared audio context plus per-session sink/microphone handling and encoded packet queues | Host audio restoration differs from isolated-session audio cleanup |
| Most subsystems | Read configuration globals; communicate through `mail::man` and `task_pool` | Globals are existing coupling, not an invitation for a new service locator |

Examples of shared state that require explicit treatment in later work:

- `globals.cpp`: `mail::man`, `task_pool`, `display_cursor`.
- `config.cpp`: `video`, `audio`, `stream`, `nvhttp`, `input`, `sunshine`.
- `process.cpp`: `proc::proc`, `vDisplayDriverStatus`,
  `isolated_runtimes_mutex`, `isolated_runtimes`, `isolated_launching_clients`.
- `video.cpp`: `chosen_encoder`, encoder status, capture shared contexts and
  pipeline metrics.
- `stream.cpp`: shared broadcast context with receive/microphone/video/audio/
  control threads and sockets, plus per-session thread and input state.
- `input.cpp`: `shared_platform_input` and `gamepadMask`.
- `display_device.cpp`: mutex-protected `DD_DATA` and retry schedulers.

`process.h` is also a significant include boundary: it includes Boost.Process,
JSON, configuration, `platform/common.h`, RTSP state and a platform
virtual-display header. `tests/tests_common.h` likewise imports global,
logging and platform state. A pure policy test should need neither header.

## Initialization, handoff and teardown

1. `main.cpp` initializes the mailbox, parses config and starts logging. Display
   recovery is initialized before platform/process/input guards. The task pool
   starts before shutdown handlers and the server threads. Signal handlers
   raise shutdown and can release display recovery early.
2. On Linux, stale display environment is dropped before `platf::init()`;
   `proc::init()` and virtual-driver initialization follow. Encoder probing
   may be deferred until a first display exists. Do not move probing ahead
   of this setup.
3. HTTPS launch in `nvhttp.cpp` evaluates routing once for permissions before
   consuming Hestia one-shot preparation and again with the final launch's
   display request. Both callers must retain the same policy.
4. `prepare_session_virtual_display` and isolated launch setup mark cleanup
   responsibility in `launch_session_t`. Before stream allocation, pending
   RTSP cancellation/failure releases the pending resources.
5. `stream::session::alloc` copies display/runtime identity into `session_t`
   and clears `session_virtual_display_cleanup_pending`: cleanup has moved
   from the pending launch to the stream. RTSP uses `lifecycle_mutex` and
   `cancelled` around cancellation/final handoff.
6. A stream destructor disconnects its isolated runtime, which can remain
   alive for resume unless `terminate_on_pause` applies. A shared-host
   session-scoped display is removed by the stream destructor instead.
7. `proc_t::terminate_isolated*` removes an entry under the runtime-registry
   mutex, then stops it outside that lock. `stop_isolated_runtime` guards
   against duplicate stopping, stops the process/unit, runs undo commands,
   releases audio/input/device resources, then removes the virtual output
   and owned assets.
8. Normal main shutdown joins the three server threads before stopping and
   joining the task pool. Local guards and their destructors then provide
   the remaining cleanup. This inventory does not certify every error path;
   preserve the existing order until independently characterized.

Frame ownership is a separate, sensitive boundary. `VDISPLAY::HermesKmsFrame`
and `HermesKmsCursor` expose owned DMA-BUF and sync-file FDs with `close()`;
`wl::dmabuf_t` owns protocol/import resources, while
`egl::img_descriptor_t` closes its descriptor FDs at destruction. These
resources must not be reduced to unowned integer handles during later moves.

## Hardware and compositor validation requirements

| Path | Required environment | What a focused headless policy test proves |
| --- | --- | --- |
| Routing policy | Standard C++ library and pinned GoogleTest | Return values and precedence only |
| RTSP/HTTPS/stream handoff | Full host dependencies and client/session fixtures | Nothing about real negotiation or cancellation races |
| AMD / Intel VAAPI | Relevant GPU, driver, capture source and libva | Nothing about import, encoding or zero-copy |
| NVIDIA / NVENC / CUDA | NVIDIA GPU, compatible driver/toolkit and capture source | Nothing about GPU-origin imports, staging or fallback |
| Hermes-KMS | Loaded compatible driver, capture/render permissions and compositor | Nothing about fences, leases, HDR or multi-output teardown |
| EVDI | EVDI module/library and a compositor driving the virtual output | Nothing about page flips, update completion or output layout |
| KWin / GNOME / wlroots | Each actual compositor, its protocols/tools and relevant devices | Nothing about cross-compositor behavior; software Sway tests cover only their exercised layout paths |
| Isolated Gamescope / KWin | Runtime tooling, broker/session permissions, audio and input backends | Nothing about process, display, input or audio isolation |

Consult `docs/compatibility.md` for existing product reports. No support or
performance claim is added by this inventory.

## First extraction: session routing policy

The initial component is the pure decision currently implemented by
`proc::resolve_session_route` near the start of `src/process.cpp`. It reads
only app preferences, capability/display-request booleans and a global method.
It does not need process execution, JSON, RTSP internals, a compositor or GPU.

The intended boundary owns **only the decision**, with explicit value inputs
and a `session_route_e` output. It owns no runtime, display, FD, thread or
configuration storage. Existing `ctx_t` callers keep their public function
and an adapter supplies the small input snapshot. Borrowed strings must remain
valid for the synchronous call; no state or references are retained.

Preserve these observed priorities, including fallback for unrecognized values:

| Priority | Condition | Result |
| ---: | --- | --- |
| 1 | Isolation capability disabled, or app requests `host-session` | Shared host |
| 2 | Explicit app method `kwin_virtual` | Isolated desktop, even without a requested virtual display |
| 3 | Explicit app method `gamescope` | Isolated application, even with an empty command |
| 4 | No virtual-display request for a default/unrecognized app method | Shared host |
| 5 | Default/unrecognized method with `extend`, `mirror` or `exclusive` layout | Shared host |
| 6 | Remaining cases: global `kwin_virtual`, or empty app command | Isolated desktop |
| 7 | Remaining cases | Isolated application |

Planned files: a small `src/session_route.h/.cpp` pair, its entry in the common
source list, the existing process adapter, focused GoogleTests and their
standalone CMake target. Existing process/RTSP/config-migration tests remain.
No persistent keys, defaults, runtime-resource ownership, protocol, capture,
encoder, CUDA or language/tool minimum changes are part of this extraction.

Validation: characterize the priority table and edge cases, retain full-host
adapter tests, build the actual new production unit with the pinned GoogleTest,
and exercise it under AddressSanitizer/UndefinedBehaviorSanitizer. Full-host CI
and real hardware evidence remain separately required. The next safe work
after this component is validated is another small policy/ownership boundary,
not moving the isolated-runtime registry wholesale.
