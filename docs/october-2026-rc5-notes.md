# Hermes — October 2026 RC5

RC5 improves physical monitor restoration, Linux session cleanup, automatic GPU selection and pairing status feedback.

## Changes since RC4

- Restore KDE physical outputs through KScreen and verify the applied layout before releasing the virtual output.
- Preserve recovery state after partial failures, retry deferred restoration, and delay a new virtual display while recovery is pending.
- Keep display event delivery available during compositor operations and bound helper and application cleanup waits.
- Improve selection of the GPU used for encoding and probe encoders without unnecessarily waking physical monitors at startup.
- Show PIN submission as pending until verification completes, including updated English and Brazilian Portuguese feedback.
- Add regression coverage for monitor restoration, process cleanup and network lifecycle handling.

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
sudo pacman -U ./hermes-streaming-0.6.0+*rc5.*-1-x86_64.pkg.tar.zst

# Fedora 44: select fc43 instead on Fedora 43
sudo dnf install ./hermes-0.6.0+*rc5.*-1.fc44.x86_64.rpm

# Ubuntu 24.04: select ubuntu26.04 instead on Ubuntu 26.04
sudo apt install ./hermes_0.6.0+*rc5.*_ubuntu24.04_amd64.deb
```

Reload and restart the installed user service after updating:

```bash
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

## Testing

Please check disconnect/reconnect, ending applications, restoring one or several physical monitors, exclusive/extended layouts, controller and touch input, and isolated-session audio/microphone behavior.

PyroWave remains an explicit SDR option for compatible clients on fast local networks. Use the existing codecs for HDR. Hardware testing remains part of release-candidate validation.

Hestia Android RC4 is the new Android companion candidate. The existing Hestia desktop and Hermes-KMS candidates remain available separately.

## Build verification and support

Publication requires successful host tests, native package builds and secure-loader checks. `RC_MANIFEST.json` identifies the exact source commit, workflow and payload hashes.

Include the RC/build version, distribution, GPU/driver/kernel, client, codec and relevant logs when requesting help.

Support is also available on our new [Discord server](https://discord.gg/M5dYFuhjqP). Responses may take time because the server is new, the community is small and time zones differ.
