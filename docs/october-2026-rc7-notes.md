# Hermes — October 2026 RC7

RC7 is mainly a fixes candidate on top of RC6. The client's microphone in isolated sessions has much less delay, Steam games stay inside their Gamescope session, and a detached desktop keeps Steam and the game when the client leaves and comes back. It also adds a capture check before each stream, a downloadable diagnostic report and covers that tell the default apps apart.

## Changes since RC6

### Fixes

- **Isolated-session microphone delay cut from about 170 ms to about 35 ms** (#89). The session's microphone is now a PipeWire source that Hermes feeds from a short jitter buffer (30 ms, at most 80 ms) instead of a pipe tunnel that always held 171 ms. Hermes now links libpipewire, and the packages depend on it.
- **Steam games stay in the stream in Gamescope's Steam mode** (#90). When the host's Xorg had been restarted on a display above a free one (for example `startx -- :1`), Gamescope's two Xwaylands were not adjacent and Steam opened its games on the host's desktop, outside the stream. Hermes now holds the free X displays below Gamescope's for the session's lifetime.
- **A detached desktop keeps Steam and the game when resumed** (#91). Leaving the stream with a game that holds the pointer made the desktop's Xwayland exit about eight seconds later, taking Steam and the game with it. The desktop now keeps its pointer and keyboard between streams, and a desktop that is only slow to answer is no longer replaced.
- The Troubleshooting page shows "No active stream" after a stream ends, instead of the last stream's figures.

### Small additions

These are contained, and were accepted after the RC6 freeze:

- **Capture check before a stream** (#93). Before the client is told the stream is ready, and while no other stream is running, Hermes captures one real frame from the display the stream will use, converts it on the encoder's GPU and encodes it. A step that fails refuses the launch with a message naming it, for example *"Capture could not open display [0] via kms"*, instead of a black stream, and stops what that launch had started. Each check logs one `Capture readiness:` line with the capture, size, rate, encoder and GPU.
- **Diagnostic report** (#94). Troubleshooting → **Diagnostic report** downloads one text file with the machine, GPUs, devices, displays, Hermes sessions, network links, a timeline of recent streams with CPU and GPU figures, warnings, kernel messages, settings with secrets removed, and the recent log. Hestia Android RC5 sends it from the PC menu. **The report names clients and their addresses: read it before posting it publicly.**
- **Covers that tell the default apps apart** (#92): Low Res Desktop, Steam Big Picture and Gamescope Steam Session have their own covers. An existing app list is updated once, and only for entries still exactly as Hermes shipped them.
- `GET /api/hestia/v1/stream/info` gives the result of the capture check to the client whose launch ran it. Hestia Android RC5 shows it for a few seconds when a stream connects.

## Behaviour changes to check

- Every launch now runs the capture check first. A display that shows nothing new for 5 seconds is not treated as broken: the launch goes on after those 5 seconds, and the log says so.
- If the check refuses a launch that you know works, set `ignore_encoder_probe_failure = enabled`. A failed check is then logged as a warning and the launch goes ahead. Please report it, with a diagnostic report attached.
- Hermes now needs libpipewire at runtime on every distribution. Any desktop that uses PipeWire already has it.

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
sudo pacman -U ./hermes-streaming-0.6.0+*rc7.*-1-x86_64.pkg.tar.zst

# Fedora 44: select fc43 instead on Fedora 43
sudo dnf install ./hermes-0.6.0+*rc7.*-1.fc44.x86_64.rpm

# Ubuntu 24.04: select ubuntu26.04 instead on Ubuntu 26.04
sudo apt install ./hermes_0.6.0+*rc7.*_ubuntu24.04_amd64.deb
```

Restart the installed user service after updating:

```bash
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

## Testing

Please check:

- Isolated sessions: the microphone's delay and quality, also over Wi-Fi and after resuming a stream.
- Gamescope Steam sessions with the host on Xorg, including after restarting X, and on Wayland as before.
- Detached Steam with a game running: send the client to the background for a minute, then resume.
- Launches of every kind (Desktop, Low Res Desktop, Steam Big Picture, Gamescope Steam Session, Detached Desktop and Detached Steam, isolated apps). The log's `Capture readiness:` line should name the display, encoder and GPU you expect.
- **NVIDIA and Intel GPUs**: the capture check and the report's GPU figures have only been tested on AMD. A launch refused on NVIDIA or Intel is exactly what this RC needs to hear about.
- The diagnostic report from the Web UI and from Hestia Android RC5, and that it holds no password, token or key.
- Two clients at once: the second client's Hestia should show no stream info rather than the first client's.
- As before: Detached Desktop and Detached Steam, controllers, disconnect/reconnect, ending applications and restoring physical monitors.

## Known limitations

RC6's known limitations still apply. In addition:

- The capture check runs only while no other stream is running, so a second client's launch is not checked, and its stream info is empty.
- A launch onto a display that produces no new frame waits up to 5 seconds for the check before it continues.

PyroWave remains an explicit SDR option for compatible clients on fast local networks. Use the existing codecs for HDR. Hardware testing remains part of release-candidate validation.

The Hestia Android, Hestia desktop and Hermes-KMS candidates remain available separately; the stream info and the report's share button need Hestia Android RC5.

## Build verification and support

Publication requires successful host tests, native package builds and secure-loader checks. `RC_MANIFEST.json` identifies the exact source commit, workflow and payload hashes.

Include the RC/build version, distribution, GPU/driver/kernel, client, codec and relevant logs when requesting help. The diagnostic report covers most of that in one file.

Support is also available on our [Discord server](https://discord.gg/M5dYFuhjqP). Responses may take time because the community is small and time zones differ.

Thanks to d3adb5 for the microphone and Xwayland fixes, and to PunkUnity for the detached desktop fixes, the capture check, the diagnostic report and the new covers in this candidate.
