# Changelog

All notable changes to Hermes are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Add new entries under **[Unreleased]** as you work. When you cut a release,
run `scripts/bump-version.sh <major|minor|patch>` — it moves everything under
[Unreleased] into a new dated, versioned section automatically.

## [Unreleased]

### Notice

- **Independent client sessions are being re-evaluated and are not recommended.**
  The prototype behind `hermes_kms_isolated_sessions` will change in ways that
  are not backwards compatible; a setup built on it now is likely to need
  rebuilding. What is known to be broken or unfinished, beyond the experimental
  label it already carried: a session composites in software rather than on the
  GPU, because neither compositor Hermes ships a profile for can be given a
  render node separately from its display device on a Hermes-KMS card; nothing
  bounds what a session may consume, so one client can starve the host; a
  session that was given a Unix account of its own still hears the host's audio;
  and Remote Input is disabled. A full desktop and simultaneous real clients
  have not been validated. The pieces below still landed and are still worth
  having - they are what the re-evaluation is being done on top of - but the
  feature as a whole is not ready to be used.

### Security
- The Game Mode console's bearer token is now rejected for WAN peers even when
  normal Web UI password/cookie logins are configured to allow WAN access. The
  token remains valid from this machine and private/local networks.

### Added
- A session compositor profile for labwc, which does not yet help a Hermes-KMS
  session and says so. Hermes shipped one profile, weston, and weston
  takes KMS nodes only - for its display device and for its rendering device
  both. The sole KMS node backed by a real GPU belongs to seat0, which is the
  one seat a session on a private DRM seat cannot open, so every isolated
  session composited on llvmpipe. That was documented as a limitation because
  weston has no way to express the split; wlroots does. It takes the display
  device and the render device separately, and a render node carries no seat
  assignment, so the session scans out on its own Hermes-KMS card and renders on
  the real GPU. `hermes_kms_session_compositor = labwc` selects it.

  What made it a profile rather than a patch was already there; two things were
  missing. wlroots opens DRM devices through libseat, whose logind backend
  resolves the calling process's own logind session - and a session started as a
  transient unit has none, on a seat that is not seat0 in any case. The profile
  asks for libseat's noop backend, which opens the path directly; the permission
  to do so is the per-card `access_uid` the driver already sets to the session's
  account, and DRM master follows from being the first to open a card nobody
  else holds.

  And labwc reads a directory rather than a file, so a profile can now name a
  file inside one - the directory is created, made traversable by the session
  account, and reaches the command line as `{config_dir}`. What Hermes writes
  into it is labwc's `autostart`, which pins the mode the client asked for with
  `wlr-randr`. That is the same workaround weston.ini carries and for the same
  reason: Hermes-KMS up to 0.5.0 marks two modes preferred, so a client asking
  for 1280x720 gets a 1080p session unless the mode is named. It is best-effort
  and `wlr-randr` is not in `requires` - a session that comes up at the wrong
  resolution is worth having, and one that refuses to start because a helper is
  missing is not.

  What a guest run then showed is that the blocker moves rather than lifts.
  labwc does open the private-seat card - `LIBSEAT_BACKEND=noop` answers the
  seat question - and does accept the render split, and does run the generated
  autostart. It then fails every swapchain, because wlroots allocates an
  output's scanout buffers through GBM on the *display device's own* render
  node: the kernel pairs `/dev/dri/cardN` with a `renderDN` of the same driver,
  and Hermes-KMS's is capture-only with no Mesa driver behind it, so Mesa falls
  back to kms_swrast and allocates with `DRM_IOCTL_MODE_CREATE_DUMB`, which the
  kernel refuses on any render node. weston never meets this because it
  allocates dumb buffers on the card node, where they are permitted - which is
  the same reason it ends up on llvmpipe.

  So the profile ships as what it is: correct for a KMS device whose render node
  Mesa can drive, and documented at the top of the file as not that for
  Hermes-KMS. `weston` stays the profile to use there, and closing this needs a
  driver that exposes a render node Mesa can drive, or none at all - not
  anything in Hermes. The `labwc.conf.example` that described the route nobody
  had taken is gone; taking it is what found the wall at the end of it.

  The render node handed to a session compositor is now chosen the way the
  capture path chooses one, rather than by a second heuristic that was wrong in
  the same way. "The first node that is not ours" follows module load order, so
  on a hybrid machine it is as likely as not a card whose driver Mesa cannot
  load - which opens like any other and fails only at the first allocation, by
  which point the compositor is up and the session is black. A candidate now has
  to survive allocating a throwaway buffer before it is believed, and
  `adapter_name` is honoured first, so a machine where the search still guesses
  wrong has somewhere to say so.

- Hermes speaks `ext-image-copy-capture`, the protocol that replaced
  wlr-screencopy. A session is capturable only through a protocol its
  compositor implements, and wlr-screencopy is a wlroots protocol: Hyprland,
  labwc and sway offer it, and the compositors that never did - GNOME among
  them - had no capture path here at all. The new backend is what those
  sessions are captured through.

  Nothing that works today moves. Where a compositor offers both, screencopy
  stays the default, because every deployment on it is on that path already and
  a working stream is not worth trading for a newer protocol.
  `HERMES_WAYLAND_CAPTURE=icc` or `=screencopy` forces one, which is how the
  other gets tested on a session that offers both.

  The two are asked for a frame differently, and the difference shows in the
  logs. Screencopy negotiates a buffer per frame; an image-copy-capture session
  negotiates once - size, format and the modifier list - and then hands out
  frames, so a frame arrives only when the output is next presented. A desktop
  with nothing moving on it therefore produces no frame, which reaches the
  encoder as a timeout and re-sends the previous one, exactly as an idle
  screencopy capture does.

  Two things the protocol gives that screencopy could not. The compositor names
  the DRM device it renders on, which is the question `init_gbm()` otherwise has
  to guess the answer to on a machine with two GPUs - it is taken as the answer,
  after the same allocation probe every other candidate gets, so a compositor
  that names a device nothing can allocate on still falls through to the search.
  And it offers format modifiers, which on AMD means the buffer that comes back
  can have more than one plane; every plane is shared, because a compositor
  handed only the first plane of a compressed buffer fails the frame with no
  reason attached and nothing to say a plane was missing.

  The protocol sequence follows Dregu's draft for upstream Sunshine
  (LizardByte/Sunshine#4788), credited in the source beside the three places
  this departs from it. Reported in #29.

- A console for Game Mode, `hermes-gamemode`, added to Steam as a non-Steam
  shortcut and launched from the library like a game. A machine that boots
  straight into Game Mode has no desktop to open the web UI on and no keyboard
  to type a password into, and the two things Hermes occasionally needs from
  the person who owns it - the PIN for a new device, and a restart - were
  reachable only by leaving Game Mode for a desktop, which is the one thing an
  appliance should never ask.

  It is a window, so it is a separate program from the streaming host: a
  service has no business owning a window, and a console that crashes must not
  take a stream down with it. It costs no new dependency - Qt is already
  required by the tray and libcurl by the host.

  Everything is a large button in a single column or a single grid, so the
  D-pad moves between controls and `A` presses, and the same layout is usable
  when Steam Input presents the controller as a mouse instead. Nothing here
  opens a gamepad device, so it needs no permissions and behaves identically
  once a keyboard is plugged in. When a device is waiting to pair the console
  notices on its own and offers a numeric keypad for the four digits, rather
  than expecting somebody to go looking for the pairing screen.

  It authenticates with a token Hermes now publishes at
  `$XDG_RUNTIME_DIR/hermes/local-api.token`, mode `0600`, created private
  before it is written and removed at shutdown. A process that can read it
  already runs as the user who owns Hermes' credentials file, config and
  process, so the token grants nothing that was not already available - it
  removes a password prompt, not a boundary. Requests carrying it are still
  subject to the same origin check as any other, and the token is never
  accepted over anything but the existing HTTPS listener.

  `GET /api/gamemode/status` answers the whole screen in one call, including
  `pending_pair`, which is true only while a client's request is parked waiting
  for its PIN.

- Hermes speaks the Hermes-KMS UAPI v13 surface. A v11 driver is still the
  floor and behaves exactly as before; a newer one lights up three things.
  Session bindings are revoked before the output is disabled, so a capture
  worker blocked in `WAIT_FRAME` is cut loose at once with `EACCES` instead of
  finding out at its next timeout, and a bind already in flight with the token
  Hermes is about to forget can no longer land in between. The diagnostics
  endpoint gained the session's binding counters - `bound_fds`, `binds`,
  `binds_rejected`, `unbinds`, `bindings_revoked` and
  `cross_session_buffer_exports` - which is how a leaked capability becomes
  visible: one capture worker plus the descriptor that reads the counters is
  two bound fds, and anything beyond that was handed out by someone else. And a
  driver that creates cards at runtime through configfs no longer reshuffles
  the pool between two enumerations, because `device_index` is neither dense
  nor stable there; session cards are ordered by the session index their
  private seat and broker are named after, which is stable by construction.
- A virtual output the driver refuses to enable now says what the driver would
  have accepted. `GET_CAPS` carries the mode envelope, which is compiled in on
  older drivers and configurable from UAPI v13 - an administrator can raise the
  maximum to 8K or lower it under what a client asks for - so the failure log
  names the requested mode and the range around it instead of leaving `EINVAL`
  to be guessed at.
- A virtual display no longer has to come from a pool sized before anyone
  connected. Hermes-KMS gained runtime card creation through configfs, which is
  root's, so Hermes ships `hermes-kms-card-broker`: a socket-activated root
  service that creates a card on a private DRM seat when asked and removes it
  when the display goes away. With it installed, an exhausted session pool is
  answered with a new card instead of a refusal; without it, nothing changes.
  Authorization is the peer's uid from `SO_PEERCRED` against
  `/etc/hermes/card-broker.allow`, which only root can write - not the socket's
  mode, because the uid is also what the card's `access_uid` is set from, and
  that is what gives the render node to that user and no one else. The broker
  allocates the session index rather than accepting one, since the driver's
  udev rules turn that index into the seat and the seatd instance that owns it,
  and two cards sharing an index would share a seat. A card can only be removed
  by the uid that created it, cards left behind by a Hermes that died are swept
  at the next start, and one user may hold four at a time.
- Virtual displays now work on Hyprland, through Hyprland's own headless output
  rather than a virtual DRM device. Hermes asks the compositor to create one over
  its control socket, and Hyprland renders it on the primary GPU - so aquamarine
  never has to drive a display-only device, which is the thing it cannot do and
  the reason Hermes-KMS and EVDI both produce a black or frozen stream there.
  Everything after creation is the path wlr sessions already took: the client's
  mode is applied with `set_custom_mode` over `zwlr_output_manager_v1`, capture
  runs over `wlr-screencopy`, and the output is removed when the session ends so
  no stray monitor is left on the desktop. Hyprland sessions take this path
  whichever virtual-display backend is configured, because neither device backend
  can work there. It does require Hermes to run inside the session: the control
  socket is found through `HYPRLAND_INSTANCE_SIGNATURE`, and a service that
  cannot see it reports the feature as unavailable with the fix rather than
  failing at stream time.
- Hermes now probes what the running session can actually do and says so at
  startup, per feature, with the fix for anything it cannot. The classification
  answers *which* compositor; this answers *what it supports*, and the two are
  not the same question: COSMIC drives an output but cannot capture one, Hyprland
  speaks every protocol involved and still cannot keep content on a virtual
  display. The session's Wayland globals are read in a single registry roundtrip
  that binds nothing, so the probe is safe to run beside a live stream, and the
  rules that turn observations into advice are a pure function - every session
  shape Hermes has to advise is a test row rather than a machine somebody has to
  own.
- `readiness` distinguishes *degraded* and *unknown* from *unavailable*. "Works,
  but not as you asked" and "could not be determined" are different messages,
  and collapsing either into "unsupported" is how a fixable configuration gets
  read as a missing capability. A session with no window system attached now
  reports unknown for everything instead of inheriting X11's answers - that
  path previously told such a process every feature was ready.
- Hermes checks whether Hermes-KMS session cards carry a private DRM seat.
  Isolation has two halves installed separately: Hermes' own udev rules put a
  session's input devices on a private seat, and the driver's
  `70-hermes-kms-session-seats.rules` does the same for its DRM cards. With only
  the first in place a session starts, gets isolated input, and shares the host
  compositor's screen, because a card with no `ID_SEAT` is on seat0 where the
  host claims it. That was previously invisible: the seatd socket was validated
  and the seat assignment never was.
- Compositor classification now names the wlroots family and COSMIC instead of
  reporting them as unknown. sway, wayfire, river, labwc and niri take one
  strategy - plain `wlr-output-management`, no per-compositor workaround - so
  they are one class rather than one case each, and a new wlroots compositor
  needs no code beyond that list. The distinction matters because "unknown" has
  to keep meaning *Hermes cannot advise this session*: diagnostics previously
  could not tell a sway session, which is expected to work, from a desktop
  nobody has ever classified. `wlroots` is also matched as a family name, but
  only after every token has been read, because a desktop list is not ordered by
  specificity - Hyprland ships `Hyprland:wlroots`, and the reverse order must not
  turn the one compositor with a dedicated strategy into the generic family.
- A session that names no desktop is now classified from the control socket its
  compositor exports. `SWAYSOCK` and `WAYFIRE_SOCKET` join the existing
  `HYPRLAND_INSTANCE_SIGNATURE` fallback, because a compositor started from a TTY
  with `exec sway` leaves both `XDG_CURRENT_DESKTOP` and `XDG_SESSION_DESKTOP`
  empty - and that is precisely the setup a user reaching for virtual displays
  tends to have.
- The `output_layout_backend` reported by diagnostics names whichever compositor
  is driving `wlr-output-management`, not only Hyprland. The protocol is not a
  strategy, and a bug report that says COSMIC is a different report from one that
  says sway.
- Hermes now captures Hermes-KMS cursor-plane updates independently from primary
  frames and composites premultiplied ARGB cursors in both zero-copy and CPU-copy
  capture paths. The implementation validates fences, buffer layouts and update
  generations, handles hidden and edge-clipped cursors, and adopts the generic
  file-descriptor-scoped session-token contract. This path requires Hermes-KMS
  0.4.x with UAPI v11 and is intended for Hermes 0.6.0.
- Maintainer announcements on the Web UI home page. A known issue can now be
  published where the people hitting it will actually see it, instead of being
  rediscovered and reported once per user. Messages come from a `feed.json` on
  the repository's `feed` branch — deliberately off `main`, because a commit to
  `main` runs CI, force-moves the `nightly` tag and republishes the nightly
  release, which would announce a phantom new build to every installation. A
  message can target a version range, target only nightlies, expire on a date,
  and be dismissed; bumping its `revision` brings it back after an edit. The
  feed is untrusted input throughout: every field is validated, links must be
  absolute `http(s)`, and the text is rendered as text. `scripts/publish-feed.sh`
  edits and publishes it, validating first because the Web UI drops entries it
  cannot parse without saying so. Users can turn the whole thing off with the
  new `announcements` option, which also stops the outbound request.
- The compositor an isolated session runs is no longer weston by name in the
  source. `hermes_kms_session_compositor` selects a profile file - the command
  line, the environment it needs, and whether Hermes names its Wayland socket or
  discovers it afterwards - read from `~/.config/hermes/session-compositors/`
  before the profiles Hermes ships, so trying one out never needs root. Only the
  command line ever differed between compositors: the readiness wait polls the
  Hermes-KMS driver rather than the compositor, and one that names its own
  socket was already found by the code that handles gamescope. So a different
  compositor is a file, not a patch. `weston` is the profile Hermes ships and
  tests; a `labwc.conf.example` beside it documents the format and is supported
  by nobody.

  Two things are worth knowing before swapping one in. The readiness wait proves
  less than its name suggests - the driver reports the configured mode as soon
  as the output exists, so it clears before any frame is drawn, and a compositor
  that fails to start is caught by the process-exited check beside it rather
  than by the wait. And weston accepts KMS nodes only, for both its display and
  its rendering device (`--drm-device=renderD128` is refused with "is not a KMS
  device"), while the only KMS node backed by a real GPU belongs to seat0 -
  which a session on a private seat cannot open. An isolated session under
  weston therefore composites in software, on llvmpipe. A compositor that takes
  a render node separately does not inherit that limit, because a render node
  carries no seat assignment; the `{render_node}` placeholder exists for that.
- The host can see the isolated sessions running on it, and end one. There was
  no answer to "what is running on this machine": the client list is who may
  connect, and the streaming session list is who is connected, while an isolated
  session is neither - it outlives the stream that started it so a client can
  come back to the desktop it left, which is also how one comes to be running
  with nobody attached and no way to notice. `GET /api/sessions/list` reports
  each one with what is needed to tell them apart and to decide about them:
  whose it is, what it is running, the compositor and seat it is on, whether it
  runs as the Hermes user or as an account of its own, whether it is still
  alive, whether anyone is attached, and for how long it has been up.
  `POST /api/sessions/terminate` ends one by client uuid, stopping the stream
  first so the client is told the session ended rather than being left holding a
  stream into a desktop being torn down under it. Ending a session unpairs
  nothing and deletes nothing - the client may connect again and get the same
  session, with its files, back.

  The clients page grew a panel for this, refreshed on its own because a
  session's state and its uptime both move with nobody touching the page. It is
  hidden where the platform never runs one rather than showing an empty list
  forever, and ending a session asks first, since it takes somebody's desktop
  and anything unsaved in it.

- An isolated session no longer streams the host's audio to its client. Capture
  had one source for the whole machine, and the way the host's own sound reached
  a stream was by moving the host's default sink onto the sink being recorded -
  right for a session that is the host's desktop, and wrong for a session that
  is somebody else's. A client heard whatever was playing on the machine,
  including its owner's, and a second client would have taken that default away
  from the first.

  Each isolated session now gets a sink of its own, named after the session and
  made with the channel layout that client asked for at launch. Its applications
  are pointed at it when they start, capture records it by name, and the host's
  default sink is never touched - so what the client hears is its own session,
  what the host hears is the host, and neither is the other's. Sessions that
  share the host's desktop are untouched: with no session sink named, every
  decision in capture is the one it made before, down to which sink it selects
  and whether it moves the default.

  Two things had to be repaired to get there. A session's applications were not
  reaching an audio server at all: the session's `XDG_RUNTIME_DIR` is rewritten
  to a private directory, and that is also where a PulseAudio or PipeWire client
  looks for its server, so they were failing to connect rather than playing
  anywhere. The host's sockets are now named explicitly. And a session that has
  a Unix user of its own is deliberately left out of this for now - it gets an
  audio server of its own along with its user manager, one Hermes cannot reach
  through a `0700` runtime directory, so a sink made on the host's server would
  be one that session never plays into and capturing it would hand the client
  silence. That case still hears the host, and says so in the log.

- An isolated session now actually runs as its own Unix user, where before it
  was a session in name only. It had a seat, a card, a Wayland socket and input
  devices of its own, but it ran under the uid Hermes runs under - so it read
  and wrote the host user's home, keys and profiles, which is the isolation that
  matters. With the session broker installed, Hermes asks it for the client's
  account and starts the session under that uid; with no broker, nothing changes
  and the session runs as before. A broker that is installed but will not hand
  out an account fails the launch rather than falling back to the host user,
  because a silent fallback is the one outcome installing it was meant to
  prevent.

  The environment stops carrying the host's session with it. It is copied from
  the Hermes process, and a copy carries the address of the host's session bus:
  an application that registers as a unique instance found the host's copy of
  itself already on that bus and handed the window to it, which is why a
  terminal opened in an isolated session appeared on the host desktop while a
  browser opened in the session. That address is also how a session would reach
  the host's clipboard, notifications and wallet. It goes, along with the host's
  audio server, credentials and identity, and systemd supplies the session's own
  in their place.

  One runtime directory became two. The session's belongs to its uid and Hermes
  cannot write it; the generated compositor config stays in Hermes' own, which
  the session can only read. Two consequences follow. Hermes can no longer list
  the session's directory to find the Wayland socket the compositor made, so the
  broker lists it and returns the name - `SOCKET` - and a compositor's log now
  goes to the journal rather than to a file Hermes owns, under
  `journalctl -u hermes-session-N`. Whether the session is alive is a question
  for systemd too, since there is no child process left to ask: `STATUS` reports
  the unit's state, and a unit that is still starting counts as running rather
  than as one that already died.

  What runs inside the session - the desktop application, the detached commands
  - runs there as well, each as a unit bound to the session's with `PartOf`, so
  stopping the session still takes all of it down the way terminating the
  process group used to.

- An isolated session can be given a Unix account of its own.
  `hermes-session-broker` is a small root service, socket-activated like the
  card broker and authorized the same way - the peer's uid from `SO_PEERCRED`
  against a root-owned allow file - that maps a paired client to an account and
  creates it on first use. Separate uids are the only isolation on Linux the
  kernel actually enforces; everything softer is a convention a determined
  client walks around. Accounts are never removed implicitly, because a save
  game outliving an accidental unpair is worth more than a tidy passwd file,
  and `--max-sessions` bounds how many may exist - eight by default, matching
  the driver's seat pool.

  The account name is derived, never received: a client-supplied identifier is
  validated to hex and dashes, used only as a lookup key, and never reaches a
  path, an account name or a command line. No shell is involved anywhere in the
  broker. The account has no shell of its own, no sudo, and is in none of
  `wheel`, `video`, `input` or `render`; its GPU access comes from the
  Hermes-KMS card's per-card `access_uid`. Its home is `0700` under a root that
  is traversable but not listable, so one session cannot enumerate the others.

  It is in `hermes-session`, and the polkit rule shipped beside it refuses that
  group every action. That rule is not optional hardening: a session on a local
  seat counts as "active and local" to polkit, a category a stock desktop hands
  well over a hundred passwordless actions - powering the host off, mounting its
  disks, unlocking encrypted volumes. The broker refuses to create an account at
  all while the group is missing, so an account cannot come to exist on a
  machine where the rule is not yet in place.

  Provisioning is no longer all it does: `START` runs a session under that
  account and `STOP` ends it. Hermes runs as the person whose machine it
  streams, so it cannot become anybody - the broker can, and asks systemd for a
  transient unit with `User=` set to the session account rather than forking one
  itself. That distinction is the whole design. A process this broker forked
  would inherit the confinement the broker is under, which forbids a network,
  forbids writable-executable memory and hides `/home`; no desktop survives
  that, and a child cannot shed a namespace or a seccomp filter. A unit PID 1
  starts is confined by its own settings instead, gets a cgroup and a lifetime
  of its own, and is reaped by systemd - so a broker restart is not a dropped
  desktop. `systemd-run` is asked rather than the bus method called by hand, for
  the reason `useradd` is asked rather than `/etc/passwd` written.

  The session gets a runtime directory and a user manager - and so a session
  bus, a Wayland socket and a PipeWire socket - from lingering, which `START`
  enables and `STOP` turns back off, because otherwise every client that ever
  streamed would leave a manager running on the host for good. It does not get a
  logind session or a PAM stack, because its compositor takes its seat through
  seatd. `START` is the one request that carries more than a line: a command and
  an environment do not fit on one, and splitting them on whitespace is a
  quoting bug waiting to happen, so it is a block of `ARG` and `ENV` lines each
  used whole, ended by `END`. `XDG_RUNTIME_DIR` is set by the broker after the
  caller's environment, so it always matches the uid it belongs to.

  `scripts/session-broker-test.sh` exercises all of this against real accounts
  and real units in a disposable virtme-ng guest, because none of it can be
  faked: what is worth asserting - a home nobody else can read, a unit running
  under another uid, lingering that goes away again, a polkit rule that denies
  that uid and throws for nobody - only exists on a machine where it really
  happened.

  The map from client to account is the broker's only memory, so it is treated
  as the record it is. A line that does not parse fails the whole read instead
  of being skipped: skipping it is silent data loss with somebody's saves as the
  blast radius, because the next write persists the file without that line and
  the client it belonged to comes back, matches nothing, and is handed a fresh
  account while its old home sits unreferenced. Writes are fsynced before the
  rename and the directory after it, so a crash cannot leave a map that is
  present, empty and authoritative. `PURGE` removes the account first and the
  mapping second - the other order loses a slot for good when `userdel` fails,
  because the account survives with nothing pointing at it and the slot search
  skips every name already in passwd - and `ENSURE` recreates an account the map
  claims but passwd does not, so neither half of an interrupted purge strands a
  client.

- Issue forms under `.github/ISSUE_TEMPLATE`, because the reports arriving
  without a log or a system description cannot be acted on and the blank issue
  box asked for neither. There is one form per kind of failure, and each asks
  only for what that kind actually needs: a bug report and a virtual-display
  report differ by the compositor's own output list, a crash report by
  `coredumpctl`, a build failure by the command and its full output.

  What every form shares is a single copy-paste shell block that prints the host
  in one go - Hermes version and commit, distribution and kernel, desktop and
  session type, GPU and driver, the Hermes/EVDI/Mesa package versions,
  `dkms status`, the loaded modules, every `hermes_kms` module parameter, the
  permissions on `/dev/dri`, the user's groups, and whether the service is
  enabled and running. Those are the answers that were missing one at a time
  across the reports so far, and asking for them one at a time is what turned
  each into a conversation. The log section names all three places a log can be
  taken from - the Web UI's Troubleshooting page has a copy button, the file is
  `~/.config/hermes/hermes.log`, and `journalctl --user -u hermes` has it too -
  says to raise the level to `debug` and reproduce first, and says what a log
  contains so it can be redacted rather than withheld.

  Blank issues are turned off, so the forms are the way in, and the chooser
  links out to the compatibility document, the troubleshooting guide, and the
  Hermes-KMS and Hestia trackers for problems that belong to them.

### Changed
- The container image no longer needs `SYS_ADMIN` to stream the desktop. The
  image sets file capabilities on `sway` and `hermes` for Steam's
  pressure-vessel sandbox, and the effective bit on `sway` is not advisory:
  `execve()` of a file whose permitted set holds a capability the process cannot
  receive fails with `EPERM`, so under rootless Podman sway did not lose a
  privilege, it did not start - and Hermes came up on a host that looked like it
  had no display at all. The entrypoint now drops those file capabilities when
  the bounding set has no `SYS_ADMIN` to give. Steam still needs the grant, and
  the README no longer claims otherwise ([#29]).
- The container entrypoint writes the detected encode GPU to `adapter_name`, so
  capture and VAAPI both use it. Both fallbacks were wrong on a hybrid host:
  VAAPI assumed `/dev/dri/renderD128` and capture took the first node that
  opened ([#29]).
- The container image's base is fully qualified (`docker.io/cachyos/cachyos`).
  Podman enforces short-name resolution and a non-interactive build cannot
  prompt for a registry, so an unqualified name simply failed to build there
  ([#29]).
- `packaging/container/README.md` documents the Podman and Quadlet path, rootless
  host device permissions (including the `uhid` module and `uaccess` rule that
  DualSense support needs), hybrid-GPU node selection, and why the headless
  deployment raises no virtual-display warning ([#29]).
- A nightly build now carries the commit it was built from in its own version:
  `0.5.1+abc1234` rather than a bare `0.5.1` indistinguishable from the release.
  The boot log, `/api/config` and the home page all show it, so a report from a
  nightly identifies its exact source. `+` is SemVer build metadata, so it sorts
  equal to the plain version rather than below it — a nightly is 0.5.1 plus
  commits, not a 0.5.1 pre-release — and it is the one separator Arch `pkgver`,
  `dpkg` and `rpm` all accept, so the package filename, the package manager's
  version and the version the binary reports stay in agreement. A stable release
  built from a `v*` tag is unchanged and still ships the bare number.
- The compatibility record no longer treats RDNA3/Navi 3x as a suspect in the
  GNOME black-stream report. The same import succeeded from plain `udmabuf` on
  the affected RX 7800 XT, the actual Hermes-KMS imported-scanout re-export bug
  is fixed in 0.3.2, and the reporter confirmed video; the RDNA3-specific tester
  request is therefore retired ([#23]).

### Fixed
- Five defects in the ext-image-copy-capture backend, found by reviewing it
  after it landed rather than before. The worst one disarmed the protection the
  backend was written around: `icc_capture()` overwrote the reinit that an
  allocation failure had just set, so a capture that could never produce a frame
  reported a timeout instead, the failure streak never advanced, and the stream
  stayed black forever - the exact outcome the streak counter exists to end. Two
  session-negotiation failures were not counted either, so a compositor offering
  only shared memory reinitialised without limit. A frame failed for an unknown
  reason leaked its buffer and one file descriptor per plane, at frame rate, and
  a multi-plane modifier buffer is the likeliest thing to be failed that way.
  Session constraints accumulated across renegotiations instead of replacing
  them, so a format the compositor had dropped could still be allocated. And a
  reinit raised while arming the first frame was lost to the dispatch loop.
- A compositor config written into a nested directory could be left
  untraversable by the session account: only the immediate parent was made
  readable, while `create_directories()` may have built a whole chain.
- "Always create Virtual Display" now says what it does not do, and the Steam
  Big Picture example says why it cannot help there at all. The option creates a
  display; where a window opens is the compositor's decision, and Hermes never
  asks it to put anything anywhere. The shipped `Steam Big Picture` entry is a
  sharper case still: `steam://open/bigpicture` is a URL handed to the Steam
  already running on the desktop, so it asks that process to change modes and
  starts nothing - the window cannot land on a display it was never launched on.
  The entry that does what people are reaching for is `Gamescope Steam Session`,
  which runs Steam inside a Gamescope compositor of its own at the client's
  resolution and ships with the virtual display already on. Both are now
  documented, on the option and in the app examples. Reported in #31.
- The Brazilian Portuguese strings for the independent-session option were
  overwritten with English in the previous commit; translated properly.
- A crash report is now something that can be answered. The Arch package
  suppressed its own `-debug` package and the workflow filtered it out of the
  upload, so the only binary anyone could install was stripped - and the two
  SIGSEGV reports received so far arrived as lists of raw offsets that nothing
  could resolve, not even with the exact release binary in hand. The shipped
  package is unchanged and still stripped; the symbols now exist beside it, so
  `coredumpctl gdb` on a reporter's machine produces a stack with names on it.
- A Hyprland session was told its virtual displays were unsupported by a
  backend it never needed. Everything on the Hyprland path was wired -
  `selected_backend()` returns `HYPRLAND_HEADLESS`, `createVirtualDisplay()`
  creates the headless output - except `openVDisplayDevice()`, which handled
  the `none` and Hermes-KMS backends and then fell through to EVDI. A host
  without libevdi installed therefore reported "EVDI is unavailable" and
  disabled virtual-display sessions, on a compositor whose headless outputs
  need no DRM device at all. It now recognises the backend, checks that
  Hyprland's control socket is reachable, and reports itself ready without
  touching EVDI. Contributed by @RZhyvitskyi in #32.
- Hermes never started in SteamOS/CachyOS Game Mode. The user unit was
  installed only into `xdg-desktop-autostart.target`, which KDE and GNOME
  activate and `gamescope-session` does not - so on a machine that boots
  straight into Game Mode the service was reported `enabled` and stayed
  `inactive (dead)`, with the user manager never mentioning it in the boot
  journal at all. It is now also wanted by `graphical-session.target`, which
  every session type reaches, Game Mode included. A `systemctl --user restart
  hermes` had always worked, which is what made this look like a startup-timing
  or environment problem rather than a unit that nothing was pulling in
  ([#2]).

  The `ExecStartPre` that ran `systemctl --user import-environment` goes with
  it. Run from inside the unit, that command copies variables out of its own
  environment - which systemd had already set to the user manager's - so it
  could only ever re-import what was present and never bring in the
  `DISPLAY`/`WAYLAND_DISPLAY`/`PULSE_SERVER` it was added for. It had been
  finding nothing since the day it landed, and said so in the journal.
- Per-session input tagging never reached udev, so isolated sessions were not
  actually isolated by input. Hermes names every virtual device it creates for
  a session with that session's tag, in `device_phys`, and the udev rules match
  on it to put one client's keyboard, mouse and gamepad on that client's seat.
  But inputtino only honoured `device_phys` on the uhid DualSense path: every
  uinput device it made ignored the field, because `phys` reaches the kernel
  only through `UI_SET_PHYS` on the uinput descriptor before `UI_DEV_CREATE`,
  and `LIBEVDEV_UINPUT_OPEN_MANAGED` leaves no window for that ioctl. Every
  session's devices therefore looked alike to udev, which had nothing to match
  on and left them all on seat0.

  The submodule now points at a fork carrying the fix, which opens
  `/dev/uinput` itself when a definition asks for a phys and closes it with the
  device. A definition with no phys keeps the managed path exactly as it was.
- Installing Hermes no longer breaks polkit for everyone on the host. The
  isolated-session deny rule declared its callback as
  `function(subject, action)`, but polkit calls rules as `(action, subject)` -
  so what the rule read as the subject was the action object, which has no
  `isInGroup`, and every authorization check on the machine died on the same
  `TypeError`. A rule that throws fails the check outright, which overrides even
  an action the policy grants unconditionally: the person actually sitting at
  the keyboard, in no Hermes session and in no `hermes-session` group, lost
  NetworkManager's `network-control` and got "Not authorized to control
  networking" when activating a connection. The rule had also never denied
  anything, because it threw before reaching the group test.
- Wayland capture no longer streams a black screen on a machine with two GPUs.
  The capture buffers handed to the compositor were allocated on the first render
  node that happened to open, and node numbers follow module load order - so on a
  hybrid laptop that is as likely as not the discrete card the compositor never
  renders on. Opening it succeeded; every frame after it failed with
  `DRM_IOCTL_MODE_CREATE_DUMB: Permission denied`, because Mesa had quietly
  fallen back to `kms_swrast` on a node no driver would claim. What reached the
  client was a stream with working audio, working input and no picture, while
  every encoder probe reported success - a probe encodes a synthetic frame and
  never touches capture. Hermes now takes the node from `adapter_name` if it is
  set, then from `WLR_RENDER_DRM_DEVICE` if the session named one, and otherwise
  scans every render node and keeps the first that can actually produce a buffer
  ([#29]).
- A Wayland capture path that cannot work now stops with the reason instead of
  retrying forever. Each failed frame ended the capture in `reinit`, which built
  a fresh capture object that considered itself the first to fail, so the same
  error repeated at frame rate for the whole session with no accumulating
  evidence anywhere - and since audio and input do not run through capture, the
  symptom was an interactive black screen rather than a failure. Buffer failures
  are now counted across reinits, the diagnosis is logged once instead of forty
  times a second, and a run of them ends the capture with what went wrong
  ([#29]).
- An unrecognized value in `hermes.conf` is no longer discarded in silence.
  Restricted keys kept their default and said nothing, so the config file stated
  one thing while every consumer behaved according to another - a typo, or a
  value written by a different version, was indistinguishable from never having
  set the key. The value, the key and the accepted alternatives are now logged.
- Hosts with no virtual-display device are no longer judged as broken EVDI hosts.
  `virtual_display_backend` accepts `none` (and reads the container image's
  `headless` as the same thing), and the readiness gates ask whether EVDI is
  selected rather than whether Hermes-KMS is not. A container install previously
  read as "EVDI backend, library missing, device unconfigured", which raised a
  home-page warning, offered an installer that could not run, and pointed at a
  `modprobe` that changes nothing there ([#29]).
- The EVDI installer is no longer offered where it cannot work. `/api/evdi/install`
  now refuses when EVDI is not the configured backend, and inside a container,
  where there is no polkit authority to ask and where a module installed in the
  container would match no kernel and drive no display ([#29]).
- The EVDI setup button rendered as the raw string `config.evdi_setup_button`.
  The key lived in the `apps` section of the locale files while the UI read it
  from `config`; the `apps` section's whole copy of the EVDI strings was dead and
  is gone ([#29]).
- The Wayland capture path no longer leaks a file descriptor per reinit.
  `gbm_create_device()` borrows the fd rather than adopting it, so destroying the
  device left it open - once per capture object, and a failing capture built a
  new one every frame.
- Configuring with CUDA enabled no longer reports "CUDA not found" on a machine
  that has the toolkit installed. Arch keeps nvcc in `/opt/cuda/bin` and puts it
  on `PATH` from `/etc/profile.d/cuda.sh`, which a build environment that is not
  a login shell never sources, so `check_language(CUDA)` searched `PATH` alone
  and missed a toolkit sitting right there. The configure step now also looks in
  `/opt/cuda`, `/usr/local/cuda`, their versioned siblings and `CUDA_HOME`, and
  for a toolkit that still rejects a GCC newer than it supports it picks the
  newest `g++-<major>` that toolkit accepts - then puts the same
  `check_language` check to the question again, so a toolkit that really is
  absent is still reported as absent. When the failure is genuine, the error now
  names the flags that aim the build at a toolkit and the log that records what
  the check tripped over ([#28]).
- nvcc no longer warns `incompatible redefinition for option 'std'` on every
  CUDA source. `CMAKE_CUDA_STANDARD` was set after `add_executable`, too late to
  initialise the target's `CUDA_STANDARD`, so CMake derived `-std=c++23` from
  `CXX_STANDARD` and the build then appended a literal `-std=c++17` of its own -
  two conflicting flags on one command line, with nvcc keeping the last. The
  default now precedes the target and the literal flag is gone, so the standard
  is stated exactly once. The CUDA sources still compile as C++17, which is what
  the trailing flag was already selecting; `-DCMAKE_CUDA_STANDARD=` still
  overrides it.
- The shaders directory is now copied whole when the build tree cannot symlink
  it. `file(CREATE_LINK ... COPY_ON_ERROR)` on a directory created an empty one
  under CMake 4.2 and below; 4.3 copies the contents and warns until a project
  says which behaviour it wants (`CMP0205`). Hermes asks for the contents, so a
  filesystem that refuses the symlink gets working shaders rather than an empty
  directory named after them.
- The session-seat remediation names the file `make install-udev` actually
  writes. The driver moved that rule from `70-` to
  `72-hermes-kms-session-seats.rules` so systemd's own `70-uaccess.rules`, which
  sorts after any `70-hermes-*` name, stops undoing its `TAG-="uaccess"` before
  the uaccess builtin runs. Anyone following the old message was looking for a
  file that is no longer installed.
- Every Hermes-KMS card past the first one is visible again. The primary card
  behind a render node was resolved with `drmGetDevice2()`, and libdrm tells two
  platform devices apart by their MODALIAS - which is `platform:hermes-kms` for
  all of them. `drmFoldDuplicatedDevices()` therefore folded the whole pool into
  one device and answered `-ENODEV` for every node but the first it happened to
  enumerate, so `hermes-kms.1` and up were dropped during enumeration with
  "Could not resolve the primary card". Isolated sessions found no session card
  to claim and fell back to the host card, and a card the broker had just
  created never appeared, failing after its three-second wait with a complaint
  about `92-hermes-kms-access.rules` that had nothing to do with it. The lookup
  now reads the character device's own `/sys/dev/char/<maj>:<min>/device/drm`,
  which names exactly one card and cannot confuse two devices for each other.
- Starting a Hermes-KMS session no longer logs the encoding GPU search as a
  string of errors. The search walks every render node on the machine and is
  meant to reject the Hermes one - it exports DMA-BUFs and does not encode - but
  said so at error level each time it did, three or four times per session,
  immediately before reporting the capture ready. A rejected candidate is now a
  debug line; an adapter named explicitly in `adapter_name` still fails loudly.
- Turning on isolated sessions no longer empties the Audio/Video settings page.
  Its warning names the per-device unit as `hermes-kms-seatd@N.service`, and
  vue-i18n reads a bare `@` in a message as the start of a linked translation
  key; `N.service` is not one, so compiling the message threw "Invalid linked
  format" the moment the alert first rendered and Vue tore the whole tab down,
  leaving the Save button alone on an empty page. The `@` is now escaped as the
  literal vue-i18n renders verbatim, so the unit name reads exactly as before.
  A sweep of the other 10,211 strings for the same class of breakage found one
  more: `apps.loading` in Portuguese had been mangled to `Carregandochar@@0`
  somewhere in the translation pipeline, which would have blanked the
  Applications page for anyone running that locale. It reads `Carregando...`
  again.
- An isolated session now actually starts its compositor. The weston and
  gamescope command lines were assembled with every interpolated value passed
  through a shell-quoting helper, but `run_command()` hands the string straight
  to boost::process, which splits on whitespace and never interprets shell
  syntax - so nothing ever stripped those quotes and weston was launched with
  `--log='/run/user/1000/hermes/sessions/hermes-s1/weston.log'`, apostrophes
  and all. It failed to open a path that does not exist, exited about six
  milliseconds in, and the control loop saw no process running and tore the
  session down before the client sent its first ping, which is why every
  attempt ended in `handshake_failed` followed by `Initial Ping Timeout`. The
  quoting is gone; the values interpolated there are generated identifiers and
  a path under `XDG_RUNTIME_DIR`, none of which carry whitespace.

## [0.5.1] - 2026-08-22

### Added
- Hermes now classifies the Wayland compositor it is running under instead of
  treating "wayland" as one case. KWin is configured through `kscreen-doctor`,
  Mutter only through `ApplyMonitorsConfig`, and Hyprland accepts the output
  through wlr-output-management and then cannot drive it — three
  strategies that differ in kind, previously selected by substring-matching
  `XDG_CURRENT_DESKTOP` twice in two backends, once case-sensitively. The
  classification reads the variable token by token, so a desktop that merely
  mentions another one's name is no longer handed its strategy, and a session
  that only exports `HYPRLAND_INSTANCE_SIGNATURE` is still recognised.
- A Hyprland session streaming a Hermes-KMS display now says, before the stream
  starts, that the backend is not supported there yet and roughly why: aquamarine
  wants every GPU owning an output to host its own GL renderer, which a
  display-only device cannot provide, and the builds that do import directly
  stall on the page-flip handshake against the driver's software vblank. The
  output is still activated — the warning is a diagnosis, not a refusal.

### Fixed
- Streaming a Hermes-KMS virtual display on KDE no longer ignores the resolution
  the client asked for. Every layer below the compositor held the negotiated
  geometry — the client requested 854x480, the virtual display was created at
  854x480, and the driver synthesised that exact CVT mode and marked it
  `DRM_MODE_TYPE_PREFERRED` — but the `kscreen-doctor` invocation only enabled,
  prioritised and positioned the connector and never named a mode, so KWin took
  1920x1080 from the standard ladder the driver also advertises. Because the
  capture path reports the real scanout rather than the requested geometry, the
  client received a full-resolution stream of a display it had asked to be
  small, with nothing in the log disagreeing. The layout command now carries the
  requested mode, and retries without one — warning that the stream will not
  match — if KWin refuses it. The same omission silenced mid-session mode
  changes: `changeDisplaySettings()` returned early whenever the requested mode
  already matched what creation had recorded, which is the ordinary case, so
  neither `hermes_kms::set_output()` nor the compositor was ever told. That
  shortcut now applies only to EVDI, whose costly reconnect it was written to
  avoid.
- Applying a mode to the compositor is now one path shared by every backend
  rather than two that had drifted apart. A mid-session resolution change told
  KWin and nothing else; GNOME was covered only because `process.cpp` happens to
  call `activateVirtualDisplayOutput()` immediately afterwards, and a wlroots
  session was not covered at all. The dispatch also cannot be called with
  `vdisplay_mutex` held — resolving the connector takes that same non-recursive
  mutex — so the lock is now released first and the constraint is stated where
  the next caller will read it.
- KWin is no longer asked for a mode blind. Hermes reads `kscreen-doctor -j`
  first and confirms afterwards, which separates the three failures that used to
  arrive as one `Layout command failed`: the output is not enumerated, it does
  not advertise the geometry, or KWin accepted the request and did not keep it.
  Refresh rates are compared rounded to whole Hz, the way `kscreen-doctor`
  itself matches a `WxH@R` argument, so a connector advertising 89.991 Hz is
  driven by asking for 90 instead of being reported as not advertising it.
- Mutter is likewise confirmed rather than trusted: `ApplyMonitorsConfig`
  returning cleanly is not the same as Mutter scanning out the mode, and an
  unconfirmed mode reaches the client as the resolution mismatch that renders as
  a broken image. Hermes now polls the state back for a second and says plainly
  when something else rewrote the layout.
- Output names reaching `kscreen-doctor` as words of a shell command are checked
  where the command is built, not only where the names were read. A name that
  fails the check costs that output rather than the session, except for the
  virtual output itself, whose layout is refused outright.
- `/api/virtual-display/status` reported `desktop.kscreen_available: false` on
  every KDE session: it compared the backend label against `"kscreen"` while the
  label has been `"kscreen-doctor"`. The generic wlroots label also now names the
  compositor behind it, so a report saying Hyprland is distinguishable from one
  saying sway.
- A session that falls back to the physical display now says why. Selecting a
  virtual-display backend is not the same as asking for a virtual display:
  `virtual_display_backend` and `hermes_kms_multi_output` choose which backend
  serves a request, while the request itself comes from the client
  (`virtualDisplay=1`), the client's certificate, the app's own setting, or
  headless mode. When none of those apply the virtual-display path was skipped
  silently, so a host whose Hermes-KMS panel read "ready" would stream the
  physical monitor at the wrong resolution with nothing in the log to explain
  it — reported independently twice ([#25]).
- Streaming a Hermes-KMS virtual display on GNOME no longer sends the client a
  black image. Hermes checked whether Mutter had adopted the virtual connector
  with a single D-Bus call issued immediately after connecting it — 6 ms in the
  reported case — so the connector was reliably still absent and the session was
  declared failed. Mutter does adopt it, but asynchronously, and at whichever
  mode it prefers rather than the one the client asked for: it drove the output
  at 1920x1080@60 while Hermes captured and encoded the requested 1600x1068@90,
  so the encoder published frames the compositor never rendered. Hermes now
  waits for Mutter to probe the connector and pushes the requested mode through
  `ApplyMonitorsConfig` instead of hoping the compositor picked it, appending a
  logical monitor at the layout's right edge when Mutter adopted the connector
  without placing it. The config is verified before it is applied and is applied
  as temporary, so the user's saved layout in `~/.config/monitors.xml` is never
  rewritten ([#22]).
- GNOME no longer keeps a black phantom monitor when a Hermes-KMS connector
  comes up enabled at boot. Loading the module with `initial_enabled=1` — which
  older Hermes-KMS packages wrote into `/etc/modprobe.d/hermes-kms.conf` —
  exposes a connected virtual output before Hermes starts, so Mutter extends the
  desktop onto an output nobody is streaming to. Hermes already disconnected
  that unowned output at startup, but only KWin's layout was repaired
  afterwards; Mutter now gets the same treatment over
  `org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig`. Because that call is
  all-or-nothing, every config is submitted to Mutter's own VERIFY method first
  and abandoned when the repair would leave no monitor driven.

## [0.5.0] - 2026-08-09

### Added
- NVENC encoding from Hermes-KMS virtual displays. NVIDIA's EGL import of the
  driver's system-memory DMA-BUFs reads the wrong pages beyond the first ones,
  so NVENC sessions capture through a CPU copy of the scanout buffer and feed
  the regular RAM-to-CUDA upload path, at the cost of one memcpy per frame.
  The copy waits on the framebuffer's exported write fence and brackets the
  read with `DMA_BUF_IOCTL_SYNC`, and the frame layout is validated against the
  real DMA-BUF size before mapping. VAAPI keeps the validated zero-copy import,
  so nothing changes for AMD and Intel ([#19]).
- The Web UI updater can now opt in to the rolling Hermes nightly channel with
  `notify_pre_releases` (disabled by default). Nightly checks compare both the
  semantic version and the exact build commit so rolling builds at the same
  version are detected without repeatedly notifying an up-to-date install.
- Experimental Hermes-KMS shared-desktop multi-output support. A single Hermes
  server can assign simultaneous Moonlight clients separate driver outputs,
  capture pipelines, modes, and absolute-input geometry inside the host
  compositor session. Enable it with `hermes_kms_multi_output`; it requires
  Hermes-KMS UAPI 8 or newer loaded with enough `outputs=N` ([#10]).
- Prototype independent client sessions behind
  `hermes_kms_isolated_sessions`. A single Hermes server now allocates one
  Hermes-KMS UAPI 9 DRM card, private runtime directory, compositor,
  application process tree, capture pipeline, and tagged virtual input set per
  client. App profiles select an application-only Gamescope session or a Weston
  desktop session. Independent DRM cards and virtual input devices share stable
  per-card udev/libseat names, and each compositor is assigned the matching
  packaged private seat-broker socket. Missing brokers fail the launch with an
  actionable diagnostic instead of silently falling back to the host seat. The
  feature defaults off and remains explicitly experimental while audio and real
  concurrent-client behavior are validated ([#10]).
- A disposable VM input-isolation test creates real uinput devices for two
  sessions and verifies that udev assigns both the input parent and event nodes
  to distinct `hermes-kms-1`/`hermes-kms-2` seats.
- A runtime container image under `packaging/container`, which runs Hermes on a
  headless sway session with audio, XWayland and an optional Steam Big Picture
  session. It is the practical way to run Hermes on an image-based distribution
  (Bazzite, Silverblue, SteamOS): the userspace lives in the container and only
  the Hermes-KMS kernel module has to exist on the host. Distinct from the
  root-level `docker/`, which holds Sunshine's *build* images and produces no
  runnable host. Adapted from
  [SOVLOOKUP/hermes-sunshine](https://github.com/SOVLOOKUP/hermes-sunshine),
  offered for upstreaming by its author in [#6]; the config paths now follow
  Hermes' own directory (migrating volumes created before that), the Chinese
  package mirrors default to off, and compose builds locally rather than pulling
  a published image. It serves one session only, and does not compose with
  `hermes_kms_multi_output` or `hermes_kms_isolated_sessions` — see its README.
- Browser-console diagnostics on the login, create-password and
  change-password pages. A request that never reaches the host leaves nothing in
  the Hermes log, because it never arrived, and `fetch` rejects with a bare
  "Failed to fetch" — so these failures were invisible from every side at once.
  Each request now logs the URL its relative path resolved to, how long it took
  before failing (an immediate rejection and a stall have different causes), the
  page origin and protocol, the browser, and the short list of things the
  browser refuses to distinguish between: a refused certificate, a reset or
  refused connection, a dismissed client-certificate prompt, or an extension
  blocking it. When the host does answer, the status and response body are
  logged instead. A report can now be a copy of the console rather than a guess.

### Changed
- Hermes-KMS diagnostics now report UAPI compatibility, driver device/output
  counts, multi-output and multi-device capability, selected output numbers,
  private seat-broker readiness, and the client assigned to each active virtual
  display.
- Exclusive virtual-display mode is intentionally ignored while experimental
  multi-output is enabled, keeping physical displays and other clients' outputs
  active.
- Remote Input is rejected while independent sessions are enabled instead of
  falling through to the shared host input devices without an unambiguous
  target seat.
- Hermes keeps its configuration in its own directory. On Linux and macOS it is
  now `~/.config/hermes` (or `$XDG_CONFIG_HOME/hermes`), holding `hermes.conf`
  and `hermes_state.json` instead of `sunshine.conf` and `sunshine_state.json`.
  Hermes previously used `~/.config/sunshine`, the same directory Apollo and
  Sunshine use, so all three shared one credentials file, one apps list, and one
  set of paired clients. On first start Hermes copies an existing
  `~/.config/sunshine` into the new location and renames those two files, so
  installs carry over untouched; the original directory is left in place for
  Apollo and Sunshine. Note that all three still bind ports 47984/47989/47990,
  so only one of them can run at a time ([#14]).

### Fixed
- Virtual-display sessions no longer fail with a 503 on unprivileged Linux
  hosts. `map_display_name()` has no settings manager to consult on Linux and
  always returns an empty string, which wiped `output_name` and left the
  pre-stream encoder probe looking up an empty display. The virtual display's
  own name (e.g. `HERMES-1`) is now kept instead ([#17]).
- `verify_kms()` now accepts a present Hermes-KMS driver, so hosts without
  `CAP_SYS_ADMIN` reach the virtual-display bootstrap instead of aborting
  platform init: these displays are captured through the driver's render node
  and never needed a readable physical framebuffer handle ([#17]).
- Active Hermes-KMS virtual displays are listed in `kms_display_names()`, so
  session display lookups resolve them by name ([#17]).
- EGL is now loaded lazily in `egl::make_display()`. Platform init loads it
  after its capture-source checks, so the virtual-display bootstrap could reach
  the function with a null glad pointer and crash ([#17]).
- `EGLImage` binds now fall back to `glEGLImageTargetTexStorageEXT` when the
  GLES-style OES bind fails, and every capture path checks the result instead of
  continuing with a silently broken texture. NVIDIA's desktop-GL driver rejects
  the OES bind for foreign system-memory DMA-BUF imports with
  `GL_INVALID_OPERATION` ([#20]).
- Distro-packaged CUDA toolkits build again. Dependency include dirs resolve to
  `/usr/include`, and the resulting `-isystem /usr/include` broke GCC's
  `include_next` chain for nvcc; the directory is now marked implicit ([#17]).
- The virtual display watchdog thread is joined on shutdown instead of aborting
  the process from a still-joinable `std::thread` during global destruction.
  Teardown is idempotent, so the watchdog-failure path and the normal shutdown
  can no longer both release the same DRM fds ([#17]).
- The Web UI no longer fails to load on the `.deb` and `.rpm` packages. Both
  packages install the assets to `/usr/share/hermes`, but neither build passed
  `SUNSHINE_ASSETS_DIR`, so the path compiled into the binary fell back to the
  default and resolved to `/usr/assets` — every page the Web UI serves, starting
  with `login.html`, was read from a directory the package never creates. The
  Arch `PKGBUILD` already set the option, which is why only the `.deb` and
  `.rpm` were affected ([#15]).
- The `.deb` and `.rpm` packages now ship the OpenGL shaders the Linux capture
  path loads. The `.deb` copied only `web/` and the top-level asset files, and
  the `.rpm` copied `shaders` as the build-tree symlink CMake creates, which
  dangled once unpacked on the target machine. Both now dereference the full
  asset tree.
- A virtual display created through EVDI now runs at the refresh rate that was
  asked for. The EDID's detailed timing carried a pixel clock tabulated per
  resolution, and every entry worked out to 60 Hz, so a request for 1080p120
  produced an EDID describing a 1080p60 display — the refresh argument was
  accepted and then ignored for every resolution the table covered. The clock is
  now derived from the requested rate, which `edid-decode` reads back as the
  standard 297 MHz / 135 kHz timing for 1080p120. A rate whose clock will not
  fit the descriptor's 16-bit field (4K above roughly 66 Hz) is clamped and
  logged rather than silently wrapping to a nonsense mode.
- Logging in works in Chromium-based browsers again. The config UI listener also
  authenticates Hestia clients by certificate, and OpenSSL requires a session id
  context on any server that does that and also allows resumption — without one,
  resuming a session is a fatal error rather than a fallback to a full
  handshake. The first request of a page load negotiated a fresh session and
  succeeded, so the page rendered; the next one tried to resume and had its
  connection killed. Brave and Chrome reported `ERR_SSL_PROTOCOL_ERROR` and
  `fetch()` a bare "Failed to fetch", while Firefox was unaffected — which made
  this look like a browser quirk rather than a server bug — and it is why only
  requests issued after the page had loaded ever broke. This was the root cause
  behind the login failures in [#14].
- The create-password and change-password forms no longer freeze silently when a
  request cannot reach the host. Neither had a `catch` on its `fetch`, and the
  welcome form cleared its loading flag only on the reply path, so a request that
  never arrived left the button disabled for good with nothing shown on the page
  and nothing in the host log — the failure was invisible from both ends. Both
  now report the reason, and a non-200 reply names its status instead of always
  claiming "Internal Server Error" ([#14]).
- The `.deb` and `.rpm` packages now declare every library the executable links
  against. `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` is off, so nothing derives these
  from the binary and the hand-written lists had drifted: both were missing
  libglvnd (GLX/OpenGL), libICE, libSM, libXext and the wayland cursor, egl and
  server libraries, and the `.deb` was also missing `libva-x11`. Some were
  pulled in transitively by Qt, which is why this did not always fail, but on a
  minimal install it produced a package that could not start. The `.rpm` also
  required `libopusenc`, which Hermes does not link, while missing `opus`, which
  it does. Package names were verified against Debian trixie and Fedora 41.
- A missing Web UI asset is now an error instead of a blank page. The page
  handlers wrote whatever `read_file()` returned, and that is an empty string
  when the file is absent — which it reports only at debug level — so a build or
  package whose assets are not where `SUNSHINE_ASSETS_DIR` says served an empty
  `200`. The browser showed white, the log said nothing, and there was no way to
  tell it apart from the UI being broken. Requests now fail with a `500` naming
  the path in the log, and startup says so outright if the assets are not there,
  rather than leaving the first visitor to find out.
- Setting credentials from the command line now works when the state file is
  unreadable. `save_user_creds()` refused to write anything if it could not
  parse the existing file, so `hermes --creds` — the documented way out of a
  lost or broken Web UI password — was disabled by the exact condition that
  makes you need it, leaving no recovery short of deleting the file and losing
  every paired client. The unreadable file is now moved to
  `<state file>.unreadable` and a fresh one written, so the paired clients are
  still recoverable from the copy ([#14]).
- Activating a virtual output no longer gives up on a compositor that has not
  published its output layout yet. `configure_virtual_output()` retried a head
  that had not appeared, but treated an output-management manager that had not
  sent its first `done` event as a hard failure on the very first attempt — so a
  compositor sitting at zero outputs, such as a headless session or a container
  that has just started, lost a race against its own hotplug and reported
  "output-management returned no complete output layout". Both conditions now
  retry on the same footing. Reported by @SOVLOOKUP, who worked around it
  downstream by keeping a placeholder output alive so the layout was never empty
  ([#6]).
- The config UI no longer makes browsers prompt for a client certificate. Its
  listener also serves the Hestia API, so it requests a client certificate on
  every connection while advertising no acceptable CAs — which a browser reads
  as "any certificate will do" and answers with a selection dialog. Dismissing
  that dialog aborts the connection and the login page reports
  `Failed to fetch`. Hermes now advertises its own certificate as the only
  acceptable CA, so browsers find no match and send an empty certificate
  silently. Paired Hestia clients present their certificate regardless of what
  is advertised, so API authentication is unchanged ([#14]).
- A failed port bind now says that another host is probably already using the
  port, instead of only reporting the error. The nvhttp message also printed the
  HTTPS port twice rather than the HTTP and HTTPS pair it claimed to list.
- KDE exclusive mode now blanks every physical monitor instead of only the
  primary one. On a multi-monitor desktop the secondary screens stayed lit and
  kept showing the local session while streaming. Crash-recovery state records
  all disabled outputs with their priorities, and ending the session restores
  each of them to the priority it had before the stream ([#12]).
- Cancelling an isolated launch now signals its in-progress preparation and
  compositor wait, including the atomic handoff into the active-runtime
  registry, so a late runtime cannot appear after `/cancel` returned.
- Cancelling after HTTP launch but before the RTSP handshake now invalidates
  the pending launch, and the per-client terminate action also stops that
  client's active stream without affecting other clients.
- Re-enabled update notifications after the Hermes rebrand and pointed stable
  and nightly release checks at `MrOz59/Hermes` instead of Apollo upstream.
- Renamed the Arch/CachyOS package to `hermes-streaming` because `hermes` is an
  unrelated PAM authentication package in the AUR. The new package conflicts
  with that package and replaces only legacy Hermes streaming packages older
  than `0.5.0`; the executable, service, assets, and user configuration paths
  remain unchanged.

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

[#2]: https://github.com/MrOz59/Hermes/issues/2
[#6]: https://github.com/MrOz59/Hermes/issues/6
[#8]: https://github.com/MrOz59/Hermes/issues/8
[#9]: https://github.com/MrOz59/Hermes/issues/9
[#10]: https://github.com/MrOz59/Hermes/issues/10
[#12]: https://github.com/MrOz59/Hermes/issues/12
[#14]: https://github.com/MrOz59/Hermes/issues/14
[#15]: https://github.com/MrOz59/Hermes/issues/15
[#17]: https://github.com/MrOz59/Hermes/issues/17
[#19]: https://github.com/MrOz59/Hermes/issues/19
[#20]: https://github.com/MrOz59/Hermes/issues/20
[#22]: https://github.com/MrOz59/Hermes/issues/22
[#23]: https://github.com/MrOz59/Hermes/issues/23
[#25]: https://github.com/MrOz59/Hermes/issues/25
[#28]: https://github.com/MrOz59/Hermes/issues/28
[#29]: https://github.com/MrOz59/Hermes/issues/29

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

[Unreleased]: https://github.com/MrOz59/Hermes/compare/v0.5.1...HEAD
[0.5.1]: https://github.com/MrOz59/Hermes/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/MrOz59/Hermes/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/MrOz59/Hermes/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/MrOz59/Hermes/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/MrOz59/Hermes/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MrOz59/Hermes/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/MrOz59/Hermes/releases/tag/v0.1.0
