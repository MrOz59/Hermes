# Linux input backend: MrOz59/libvirtualhid

**Status: integration branch.** `experimental/libvirtualhid-input` uses
libvirtualhid for the complete Linux virtual-input stack. Inputtino is not
compiled or linked.

## Dependency provenance

The dependency is our fork,
[MrOz59/libvirtualhid](https://github.com/MrOz59/libvirtualhid), pinned by CMake
to commit `14ee6c4717d32b678c2983bfa6c54019b944d421`. Updating the dependency
requires reviewing and intentionally changing that SHA.

That revision was promoted to the fork's `master` on 2026-10-25 by a
fast-forward from `38904f3`. Hermes keeps the exact pin; building this branch
does not require installing a separate libvirtualhid package. Promoting the
library does not promote Hermes' integration branch or validate streaming.

The library uses C++23, Linux `uinput`/`uhid`, and libevdev. Hermes needs read
and write access to `/dev/uinput` and `/dev/uhid`; applications consuming
controller output reports also need the generated `hidraw` node. The packaged
`60-hermes.rules` grants those permissions. Never use
`lvh::BackendKind::fake` in production: it accepts events without creating
kernel devices.

## Integrated paths

| Path | libvirtualhid implementation |
| --- | --- |
| Relative and absolute mouse, five buttons, horizontal and vertical high-resolution scroll | `lvh::Mouse` |
| Keyboard and UTF-8 text | `lvh::Keyboard` |
| Generic HID, Xbox 360/One/Series, DualShock 4, DualSense and Switch Pro controllers | `lvh::GamepadStateAdapter` |
| Controller touchpad, motion and battery | cached `lvh::GamepadState` |
| Rumble, trigger rumble, RGB LED and adaptive triggers | `lvh::GamepadOutput` callback |
| Multitouch, hover and cancel-all | `lvh::Touchscreen` |
| Pen, eraser, pressure, distance, tilt and three barrel buttons | `lvh::PenTablet` |

Hermes still owns GameStream packet parsing, client permissions, keyboard
repeat and feedback messages. The adapter translates those contracts into
libvirtualhid state and reports.

## Configuration and client negotiation

The Hermes web UI's Input tab selects the emulated controller. With `gamepad =
auto`, PlayStation, Nintendo and Xbox client types select DualSense, Switch Pro
and Xbox Series respectively. For an unknown client type, `motion_as_ds4` and
`touchpad_as_ds4` can select DualSense; otherwise Xbox Series is used. A manual
profile always overrides this detection. `xone` remains available explicitly.
`linux_gamepad_backend = auto` uses uinput for generic/Xbox controllers,
preserving ordinary rumble without requiring games to open hidraw. PlayStation
and Switch retain native HID. Choose `native` explicitly for Xbox trigger rumble
and battery, with appropriately restricted hidraw access for the consuming game.
The `uinput` policy restricts automatic selection to Xbox Series and rejects
manual PlayStation/Switch profiles; it does not silently change a manual profile.
Back/Select can click the virtual DS4/DS5 touchpad using
`ds4_back_as_touchpad_click`. `virtualhid_randomize_mac` controls both
PlayStation profiles and accepts the two older DS5-specific configuration keys.

Mouse/keyboard/controller switches gate their input paths. Native pen/touch
advertisement also requires `mouse`, `native_pen_touch` and an available backend.
Native packets are rejected when disabled, except cancel-all cleanup. On the
Android client, MultiTouch sends touchscreen input; absolute/trackpad modes
send mouse input. Pen input follows its own tool-aware path. No client mode
turns missing hardware pressure or motion sensors into real measurements.

Stopping a stream releases mapped keys, all five mouse buttons, gamepad
buttons/axes and active touch/pen state only on its owning input context.
Isolated contexts are not retained after their seat/compositor is destroyed.
Sunshine's global retained-gamepad lifecycle must not be copied across private
seats; a new isolated stream gets a new context.

## Isolated sessions

Every isolated stream receives its own `platf::input_t`, libvirtualhid runtime,
mouse, keyboard and controller collection. Pen and touchscreen state remains
per client. All devices carry the matching `hermes-kms-N` physical identity:

- uinput devices expose it through `UI_SET_PHYS`;
- UHID controllers expose it through `HID_PHYS`;
- node discovery requires an exact physical-identity match; and
- an isolated keyboard or mouse never falls back to process-global XTest.

The packaged udev rule maps that identity to the private `ID_SEAT`. UHID
controller drivers create their input children two levels below the HID device
and may clear `input/phys`, so the rule propagates the seat from the HID parent
to every `input`, `event`, `js` and `hidraw` child. Device names are not used as
the isolation boundary. DualSense MACs are randomized by default; deterministic
mode includes the private session identity for both DS4 and DS5 so two seats cannot alias each
other. Switch Pro initialization also advertises a distinct, session-scoped
MAC rather than the formerly fixed protocol MAC.

Session teardown destroys only that session's runtime and kernel handles. The
shared desktop continues to use an input object with an empty session tag.

## Input access for broker-created accounts

The broker registers the compositor's `XDG_SEAT` together with the paired
client, mapped account and current UID. It persists that lease in the private
`/var/lib/hermes/session-users.map.input` file before granting any access.
Scoped applications inherit that seat and cannot replace it. The request
protocol accepts neither device paths nor UIDs for ACL operations.

The broker discovers virtual `event`, `js` and `hidraw` nodes for that exact
seat. It requires matching sysfs ancestry and `/sys/dev/char` identity, opens
the node without following symlinks, verifies its character-device number and
sysfs identity again, and edits the ACL by descriptor. It grants only the
session UID read/write access, preserving other ACL entries' effective rights.
A name reused by a different device is never sufficient authorization.

The packaged `99-hermes-session-input.rules` invokes a root-only helper after
the distribution uaccess rules and before udev publishes a new device. This
ensures already-running SDL/libinput consumers can open it on the first add
event. The helper accepts only a numbered kernel node name and uses a short
ACL lock, so another session stopping does not hold up its hotplug.

Input reconciliation also runs approximately every two seconds to retry missed
updates and recover after restart, without a new request from Hermes.
Persisted leases survive a broker restart. An isolated session
started by an older broker needs to be restarted once to register its lease.

STOP, PURGE, failed initial launches and completed sessions stop the managed
session/app units and user manager before removing the named device ACLs.
A failed cleanup retains the lease for retry and prevents PURGE from releasing
an account with outstanding access. ACL changes cannot invalidate existing
open descriptors, which is why process shutdown comes first.

The accept loop delegates work to at most 64 child processes. Account/lifetime
mutations remain serialized by the same validated file lock, while read-only
requests can finish during slow account creation or systemd shutdown. Lock
contention queues for up to two minutes; unsafe locks fail immediately. The
client gives mutations a total three-minute budget and read-only queries 15
seconds. Status replies come from systemd rather than a stale process-local
cache. An unavailable or malformed liveness reply does not by itself stop an
existing stream; teardown requires a confirmed inactive or unmapped session.
Nonblocking connect, send and receive operations share one deadline, including
a full Unix listen queue, and incomplete replies are refused.

The native broker build requires `libacl` headers and library (`libacl1-dev`
on Debian/Ubuntu, `acl` on Arch). Packages include the runtime dependency;
the Flatpak build also includes its attr/acl build dependencies. Device ACLs
do not bypass a container's device filter: native HID profiles still require
the container to expose their own hidraw nodes.

Run `scripts/session-broker-test.sh /path/to/hermes-session-broker` as root
**only in a disposable VM with systemd, udev, uinput, uhid and ACL tools**.
It installs the packaged broker and creates real accounts. The test verifies
both event and hidraw access for two UIDs, cross-seat denial, SDL consumers
started before device creation, replacement devices, persisted leases after
restart, STOP and PURGE. This is separate from
pure identity/ACL tests, which touch only temporary fixtures.
`python3 scripts/session-broker-concurrency-test.py /path/to/hermes-session-broker`
also runs the actual daemon with a private socket/map/allow-file. It creates no
accounts or devices and requires no systemd; it verifies incomplete requests,
queued mutations and immediate refusal of unsafe locks.

## Building

```bash
git clone --recursive https://github.com/MrOz59/Hermes.git
cd Hermes
git switch experimental/libvirtualhid-input
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sunshine -j2
```

The first configure fetches the pinned fork revision. There is no backend
selection option: Linux builds use libvirtualhid exclusively.

## Manual validation with a DS4 and DualSense

A physical controller connects to the **Hestia client**. Hermes creates a
separate virtual controller on the host; connecting the physical controller
directly to the host would bypass the streaming path being tested. A DS4 can
drive virtual Xbox profiles as well as PlayStation profiles. Different physical
controllers are not required to exercise each host profile, although synthetic
tests cannot prove every controller's client-side behavior.

Start with the DS4. Borrowing a DualSense adds useful coverage but need not
delay the first round. Keep the host kernel, compositor, display/capture mode,
game, Steam Input setting and stream settings unchanged between the known-good
Hermes build and this branch. Record whether each physical connection is USB
or Bluetooth; test the other connection separately if the client supports it.

Install a package built from `experimental/libvirtualhid-input`, including its
matching session broker and both packaged udev rules. A standalone replacement
of the Hermes binary does not validate the updated permissions or broker.
Keep the previous package and a copy of the Hermes configuration for reversal.
Use the native host first; the production container needs a separate round.
See [Building](building.md) for dependencies and packaging instructions. On an
AMD-only build without the CUDA toolkit, configure with
`-DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF`.

In Hermes' Input settings, test these rows one at a time. Save, restart Hermes
between rows after ending the stream, and reconnect so the virtual device is
recreated. Preserve unrelated configuration settings.

| Physical controller | `gamepad` | `linux_gamepad_backend` | Main purpose |
| --- | --- | --- | --- |
| DS4 | `auto` | `auto` | Normal negotiation; a PlayStation client type selects virtual DualSense |
| DS4 | `ds4` | `auto` | Explicit virtual DS4, native HID and ordinary rumble |
| DS4 | `xseries` | `auto` | Xbox input-event path and ordinary rumble without requiring hidraw |
| DualSense, when available | `auto` | `auto` | Second physical controller and normal negotiation |
| DualSense, when available | `ds5` | `auto` | Explicit virtual DualSense and available client extensions |

For each row:

1. In the streamed desktop, open a controller tester **inside the host
   session**, then a game with known controller support. Confirm all buttons,
   D-pad diagonals, both sticks, independent analog triggers and ordinary
   rumble. Repeat with Steam Input disabled where the game supports the device
   directly; record its setting rather than mixing the two paths.
2. Play for at least 30 minutes on the initial DS4 automatic profile. Check for
   stuck inputs, duplicate controllers, intermittent rumble and unexpected
   disconnections. Use the other rows for shorter focused checks first.
3. Disconnect and reconnect the physical controller during a stream, then
   disconnect and resume Hestia. Repeat five times. Buttons, axes and rumble
   must still work, and no input may remain held after a fresh session starts.
4. For PlayStation profiles, check touchpad click, one/two-finger contacts,
   motion and battery only when the client actually advertises and forwards
   them. Record unsupported client features as **not tested**, not passed.
   Mouse movement alone does not prove native controller touchpad reports.
   Adaptive triggers require a compatible game, client and physical transport;
   ordinary rumble does not validate them or advanced haptics.

Then test two independent clients with isolated sessions. The second client
can initially use keyboard/mouse, so a second physical controller is optional.
Verify that inputs from one client never reach the other session or the local
desktop. Start the second session while the first is playing; stop it and
confirm the first remains responsive. With two controllers available, add
simultaneous controller input and verify that rumble reaches only its owner.
If only one client is available, mark concurrent streaming as pending; the
two-account CI is not a substitute for this end-to-end check.

Finally, with no valuable work open, restart Hermes and resume a session.
Repeat once after reboot to cover installed rules and service startup. When
finished, reinstall the previous package, restore the saved configuration if
needed, and confirm that a stream still starts. Do not run the disposable-VM
account test script on the daily-use host to obtain a passing result.

For each result, record the Hermes commit/package, Hestia version and client
OS, host kernel/compositor, physical controller and USB/Bluetooth connection,
the two settings above, game and Steam Input setting, expected/observed behavior
and time of failure. Keep the Hermes log from the same attempt; for an
isolated-session failure also keep the session-broker journal. CMake configure
output must identify `Hermes Linux input backend: MrOz59/libvirtualhid`;
controller-selection messages show the profile chosen by `auto`. If more
detail is needed, repeat the failure with `min_log_level = debug`, then
restore the previous log level. Review logs before sharing them.

The merge gate is a repeatable DS4 stream, the profile and lifecycle checks,
concurrent isolated sessions, and package update/reversal without manual
permission grants. DualSense results expand that evidence. Switch/Xbox
physical-client behavior, unavailable PlayStation driver paths and the
production container remain explicitly unvalidated until tested separately.

## Validation

The startup/liveness checkpoint at `6700e5c` passed
[35 independent broker/client/ACL tests, three real-daemon concurrency checks
and 58 checks with real accounts](https://github.com/MrOz59/Hermes/actions/runs/40157668651/job/121062412179).
The [complete compiler job](https://github.com/MrOz59/Hermes/actions/runs/40157668651/job/121062440390)
passed 77 selected Hermes tests and 176 library tests; the
[kernel job](https://github.com/MrOz59/Hermes/actions/runs/40157668651/job/121065555329)
passed 32 adapter/kernel tests and four library runtime/event-backend tests.
The runtime harness additionally queries a live session during queued account
provisioning and freezes the broker's entire cgroup during SDL hotplug checks,
so background workers cannot rescue a missed udev authorization.

Artifact reuse also compares the compiler job and global configuration, not
just changed filenames. Build flags, dependency installation and compiler image
changes invalidate reuse. Policy regression cases run in CI; a manual workflow
execution always requests a fresh complete build.

Earlier checkpoint:

The 2026-10-25 checkpoint passed the complete Hermes build, 62 selected Hermes
tests and 176 library tests in the
[compiler job](https://github.com/MrOz59/Hermes/actions/runs/40151792958/job/121044822644).
The [session-account job](https://github.com/MrOz59/Hermes/actions/runs/40153673474/job/121050309949)
passed 58 checks with real UIDs, including hotplug while broker reconciliation
was paused. The [kernel job](https://github.com/MrOz59/Hermes/actions/runs/40153673474/job/121050328653)
passed 32 Hermes tests and four library runtime/event-backend tests. All seven
controller profiles completed 1000 batches at 1000 Hz without SYN_DROPPED or
cross-seat keyboard events; both Xbox event profiles delivered non-root rumble.
These results do not validate an entire streamed Hestia desktop or real games.

The kernel job gives one udev daemon exclusive control of the disposable
guest's device nodes. Native Switch evdev validation requires hid_nintendo;
when Azure omits it, CI builds that driver from the matching upstream Linux
series against the running kernel's headers. A runtime-harness or migration-note
change may reuse binaries from a passing compiler job after checking that all
compiled inputs remain identical. Source, dependency, CMake and compiled-test
changes always trigger a new complete build.

The branch workflow compiles Hermes and runs the library's identity/discovery,
UHID round-trip and fail-closed isolation tests. On a host with uinput/uhid,
the library consumer suite also verifies evdev, libinput, SDL, touchscreen,
trackpad and tablet behavior.

Pure unit tests run without kernel device prerequisites. A separate disposable
runner job loads uinput/uhid and runs the real-device smoke tests, per-seat
routing tests and an SDL consumer under an unprivileged UID. That consumer
requires an ACL for its own event node, rejects access to the foreign seat,
and must produce ordinary rumble without any hidraw node. The test grants ACLs
only inside the disposable guest; it does not prove broker provisioning of
those ACLs in production.

SDL versions can expose Xbox directional inputs as raw buttons or a hat; the
tests verify canonical button transitions independently of that layout. The
DualSense kernel-child discovery test requires the HID device to be bound to
`hid-playstation`. It explicitly reports a skip when a runner binds another
driver instead; that skip does not validate motion/touchpad child discovery.
The local VM validation below used `hid-playstation` and ran that test.

The repository's `scripts/vm-isolated-input-test.sh` validates the packaged
udev rules inside a disposable VM. Pass `build/tests/test_sunshine` to also
exercise the actual Hermes adapter with two concurrent private seats, including
seat 12 to cover dynamically allocated indices beyond the old eight-seat pool:

```bash
cmake -B build -G Ninja -DBUILD_TESTS=ON
cmake --build build --target sunshine test_sunshine -j2
# Inside a disposable guest, as root with udev running:
scripts/vm-isolated-input-test.sh /absolute/path/to/build/tests/test_sunshine
```

Library baseline validation on 2026-10-24 passed 193 tests (one xpadneo-specific
test skipped). The expanded adapter validation passed 37 local tests covering
input translation, reset, configuration, locales, coordinates and schema, and
all 25 tests in the VM adapter run. The VM run exercised Generic HID,
Xbox 360/One/Series, DualShock 4, DualSense and Switch Pro with
independent keyboard, relative/absolute mouse, touch and pen devices on
`hermes-kms-1` and `hermes-kms-12`. For each controller profile it replayed
1000 batches at a target rate of 1000 Hz; each batch contained keyboard, mouse,
touchscreen, pen and controller input. There were no evdev `SYN_DROPPED`
events, all 1000 keyboard transitions and relative moves arrived, and the
checked input events did not appear on the other session. Teardown removed
only the first session's devices; the surviving session still accepted input.

These tests verify kernel input routing and lifetime, not an entire streamed
desktop or applications running under broker-created Unix accounts. Such
applications still require device access appropriate to their account; seatd
access for a compositor does not itself grant hidraw access to a game. The
container profile deliberately excludes hidraw, so native HID controller
feedback and advanced features need separate container validation before this
branch is promoted to a release.

Physical pen hardware is not required for the synthetic pen/tablet tests. It
is still useful before a release for validating device-specific Android stylus
quirks, desktop applications such as Krita, and end-to-end feel. Likewise,
real concurrent clients remain the final integration test for compositor,
audio and lifecycle behavior outside the input library itself.
