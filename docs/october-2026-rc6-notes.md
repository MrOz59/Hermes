# Hermes — October 2026 RC6

RC6 brings the Detached desktop: a remote client can open the user's own Plasma desktop, or Steam Big Picture inside it, as a separate session that never touches the PC's monitors, keyboard or mouse. It also fixes a stream start crash.

## Changes since RC5

- **Detached Desktop and Detached Steam** (experimental, opt-in). With `hermes_kms_isolated_sessions = true` and the Hermes-KMS virtual display, the app list gains two entries, marked **DETACHED** on their covers:
  - **Detached Desktop** opens the user's own Plasma desktop (same account, files, settings and Steam library) on a private KWin virtual output. The physical monitors stay off, and the machine's keyboard and mouse never reach it.
  - **Detached Steam** opens the same detached desktop with Steam Big Picture running in it.
  - Existing app list entries are not changed. Both entries are hidden from clients while independent sessions are off, and an entry you delete stays deleted.
- The detached desktop runs at the client's resolution and refresh rate, at launch and on resume (for example 1080p at 120 Hz instead of a fixed 60).
- Controllers work in the detached desktop, in Steam and in games, through a private `/dev` that the host's seat and the user's host applications cannot open. A game keeps its controller when the client leaves the stream and comes back.
- The detached desktop has its own private audio server, so its sound never reaches the PC's speakers and the host's audio never reaches the client.
- Works with the packaged Hermes and the file capabilities its installer sets.
- The detached desktop does not start its own polkit agent, PowerDevil or BlueDevil: no password prompts that could count against `pam_faillock`, no second power manager for the monitors, and no `obexd` restart loop.
- Shared-host and independent sessions can now run at the same time. Enabling independent sessions no longer disables host multi-output or Remote Input.
- Fix premature stream timeouts and teardown watchdog crashes when a stream starts.
- The Linux packages are now built against PipeWire and `plasma-wayland-protocols`, so they include the detached desktop where the distribution's Plasma allows it. The Arch package now depends on `libpipewire`.

## Behaviour changes to check

These only apply with `hermes_kms_isolated_sessions = true`:

- `session-type: auto` now stays on the shared host session unless the app requests a virtual display with `virtual-display-layout: detached`. An app that relied on `auto` to start in an isolated Gamescope session needs `session-type: application`.
- `hermes_kms_session_compositor` now defaults to `plasma`. On a machine without Plasma 6, set it to `weston` for independent desktop sessions.

## Which download should I choose?

All application packages are for 64-bit x86 Linux PCs. Choose one package matching your distribution.

| Your system | Download |
| --- | --- |
| Arch Linux / CachyOS | `hermes-streaming-…-x86_64.pkg.tar.zst` |
| Fedora 43 | `.fc43.x86_64.rpm` |
| Fedora 44 | `.fc44.x86_64.rpm` |
| Ubuntu 24.04 | `_ubuntu24.04_amd64.deb` |
| Ubuntu 26.04 | `_ubuntu26.04_amd64.deb` |

The Arch `hermes-streaming-debug-…` package contains optional debugging symbols. `RC_MANIFEST.json` and `SHA256SUMS` record the exact build and checksums; they are not application installers.

## Install or update

Use the actual downloaded filename:

```bash
# Arch / CachyOS
sudo pacman -U ./hermes-streaming-0.6.0+*rc6.*-1-x86_64.pkg.tar.zst

# Fedora 44: select fc43 instead on Fedora 43
sudo dnf install ./hermes-0.6.0+*rc6.*-1.fc44.x86_64.rpm

# Ubuntu 24.04: select ubuntu26.04 instead on Ubuntu 26.04
sudo apt install ./hermes_0.6.0+*rc6.*_ubuntu24.04_amd64.deb
```

Reload udev rules and restart the installed user service after updating. This RC adds udev rules for the detached desktop's controllers:

```bash
sudo udevadm control --reload
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

## Trying the detached desktop

1. Use KDE Plasma 6 on Wayland with the Hermes-KMS virtual display.
2. Set `hermes_kms_isolated_sessions = true` and restart Hermes.
3. Refresh the app list on the client and open **Detached Desktop** or **Detached Steam**.

The detached desktop needs Hermes built with its private KWin transport (PipeWire and `plasma-wayland-protocols` with KDE screencast version 6). The Arch / CachyOS package always has it. A distribution whose `plasma-wayland-protocols` is older gets a package without it, and the detached apps then report that Hermes was built without the KWin transport; this applies to Ubuntu 24.04, whose Plasma is 5.27.

If the detached desktop cannot start, the client and the Hermes log say why, and at which stage.

## Testing

Please check:

- Detached Desktop and Detached Steam: launch, video, audio, keyboard, mouse and touch, and that the physical monitors stay off.
- Controllers in Big Picture and in games, including after leaving the client and resuming.
- Resuming at a different resolution or refresh rate, and Quit followed by a fresh launch.
- A shared-host app (Desktop, Steam Big Picture) while a detached desktop is running, and the reverse.
- **NVIDIA and Intel GPUs**: the detached desktop has only been tested on AMD so far.
- As before: disconnect/reconnect, ending applications, restoring physical monitors, exclusive/extended layouts, and isolated-session audio/microphone behaviour.

## Known limitations

- The detached desktop is SDR only, and its frames are copied through system memory.
- A fresh detached desktop may show a KDE Wallet prompt; dismissing it is enough, and one unlock lasts for that desktop.
- Artemis hides its on-screen controller when it goes to the background, and it has to be turned back on after resuming. A physical controller is unaffected.
- Steam Input pads stay on the host seat if a process outside the detached desktop holds `/dev/uinput`.
- Detached Steam opens Big Picture once per desktop launch. Quitting Steam inside the desktop leaves the desktop running.
- Bluetooth pairing and file transfer are done from the host desktop, not from a detached one. Bluetooth on the client device is unaffected.

PyroWave remains an explicit SDR option for compatible clients on fast local networks. Use the existing codecs for HDR. Hardware testing remains part of release-candidate validation.

The Hestia Android, Hestia desktop and Hermes-KMS candidates remain available separately.

## Build verification and support

Publication requires successful host tests, native package builds and secure-loader checks. `RC_MANIFEST.json` identifies the exact source commit, workflow and payload hashes.

Include the RC/build version, distribution, GPU/driver/kernel, client, codec and relevant logs when requesting help.

Support is also available on our [Discord server](https://discord.gg/M5dYFuhjqP). Responses may take time because the community is small and time zones differ.

Thanks to PunkUnity for the detached desktop work in this candidate.
