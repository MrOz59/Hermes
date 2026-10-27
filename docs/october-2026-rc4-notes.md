# Hermes — October 2026 RC4

RC4 fixes server-side pairing failures that could require several attempts, especially after an interrupted handshake. It includes the fixes from RC3 and is intended to be used with the reissued [Hestia Android RC3](https://github.com/MrOz59/Hestia-Android/releases/tag/october-2026-rc3).

## Changes since RC3

- Starting a new pairing attempt for the same client now replaces its unfinished handshake and releases any older request waiting for a PIN.
- Add the pending-handshake cancellation endpoint used by Android when pairing fails. Cancelling an attempt preserves already authorized clients.
- Fix session lifetime errors during handshake completion and rejection that could access a session after it was removed.
- Submit a PIN only to a session actually waiting for one, and synchronize concurrent pairing requests and PIN submission.
- Remove sensitive pairing payloads from debug logs.
- Add regressions for interrupted retries, scoped cancellation, malformed handshake data and PIN selection, alongside the existing cryptographic pairing tests.

RC3's Linux process cleanup and intentional service shutdown fixes, and RC2's capability-safe PyroWave packaging, are retained. Each package manager checks that RC4 upgrades RC1, RC2 and RC3.

## Which download should I choose?

All application packages below are for **64-bit x86 Linux PCs**. Download one matching your distribution.

| Your system | Download |
| --- | --- |
| Fedora 43 | `.fc43.x86_64.rpm` |
| Fedora 44 | `.fc44.x86_64.rpm` |
| Ubuntu 24.04 | `_ubuntu24.04_amd64.deb` |
| Ubuntu 26.04 | `_ubuntu26.04_amd64.deb` |
| Arch Linux / CachyOS | `hermes-streaming-…-x86_64.pkg.tar.zst` |

The Arch `hermes-streaming-debug-…` package contains optional debugging symbols; it is not the application installer. `RC_MANIFEST.json` and `SHA256SUMS` record build details and checksums and do not need to be installed. Android users should install **Hestia Android**, the client, on their phone, tablet or TV; Hermes runs on the host PC.

## Install or update

Use the downloaded package for your system:

```bash
# Fedora 44 (use fc43 instead for Fedora 43)
sudo dnf install ./hermes-0.6.0+*rc4.*-1.fc44.x86_64.rpm

# Ubuntu 24.04 (use ubuntu26.04 instead for Ubuntu 26.04)
sudo apt install ./hermes_0.6.0+*rc4.*_ubuntu24.04_amd64.deb

# Arch Linux / CachyOS
sudo pacman -U ./hermes-streaming-0.6.0+*rc4.*-1-x86_64.pkg.tar.zst
```

After updating, reload and restart the user service:

```bash
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

Existing authorized clients should remain paired. For a new pairing, select the PC in Hestia and enter the displayed PIN in the Hermes Web UI. If an attempt is interrupted, try pairing again; the new request replaces the unfinished attempt.

## Companion versions and testing

- **[Hestia Android RC3, reissued](https://github.com/MrOz59/Hestia-Android/releases/tag/october-2026-rc3)** includes the separate Android discovery crash fix and client-side pairing recovery. Choose its **universal APK** if unsure which architecture to use. Android startup, pairing and connection were confirmed on the maintainer's phone before that reissue.
- [Hestia desktop RC1](https://github.com/MrOz59/Hestia/releases/tag/october-2026-rc1) and [Hermes-KMS RC1](https://github.com/MrOz59/Hermes-KMS/releases/tag/october-2026-rc1) remain compatible.

Please test first-time pairing, cancelling and retrying, reconnecting after restarting both applications, and connecting an already authorized client. Report the exact host/client versions and logs from both ends if repeated attempts are still needed. The server changes have automated regression coverage; broader validation of this RC on physical devices remains open.

Continue checking Gamescope/Wine shutdown on Fedora, isolated-session audio/input, controllers and microphone passthrough. Hardware confirmation of issue #75 and the separate Assassin's Creed Origins black-screen-on-exit report remain pending. PyroWave remains an experimental SDR option for fast local networks; use the existing codecs for HDR.

## Build verification and reports

Publication requires the host suite, native packages and Fedora 43/44 + Ubuntu 24.04/26.04 secure-loader checks to pass. Installed RPM/DEB startup is tested as a regular user with capabilities enabled. `RC_MANIFEST.json` records the exact source/workflow and payload hashes; `SHA256SUMS` covers the attached files.

Include the RC/build version, distribution, GPU/driver/kernel, client, codec, reproduction steps and logs with bug reports.

Support is also available on our [Discord server](https://discord.gg/M5dYFuhjqP). Responses may take time because the server is new, the community is small and time zones differ.
