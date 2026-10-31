# Hermes — October 2026 RC3

This candidate addresses the shutdown failure reported after testing RC2 with Gamescope and Wine on Fedora 44.

## Changes since RC2

- Fix Linux process-group detection when a launcher has exited but game descendants remain alive. Hermes now checks the group itself instead of assuming that no directly waitable children means the workload has ended.
- Bound graceful and forced cleanup, preserve exit-status handling, and report termination failures.
- Treat Quit from the tray as an intentional stop of the installed user service, preventing a cleanup timeout from automatically relaunching Hermes.
- Override Fedora's timeout-abort policy so a service cleanup timeout does not produce misleading Gamescope/Wine abort coredumps.
- Add real Linux regressions covering surviving descendants, resistance to SIGTERM, forced termination, zombie reaping and protection against invalid or own process groups.

The Fedora/Ubuntu capability-safe PyroWave packaging fixes from RC2 are retained. RC3 package versions are checked against RC1 and RC2 using each distribution's native version comparator.

## Install or update

Choose the package matching your distribution. For example:

```bash
# Fedora 44 (choose the fc43 package for Fedora 43)
sudo dnf install ./hermes-0.6.0+*rc3.*-1.fc44.x86_64.rpm

# Ubuntu 24.04 (choose ubuntu26.04 for Ubuntu 26.04)
sudo apt install ./hermes_0.6.0+*rc3.*_ubuntu24.04_amd64.deb
```

Arch/CachyOS users can install the matching `hermes-streaming` package with `sudo pacman -U ./hermes-streaming-0.6.0+*rc3.*-1-x86_64.pkg.tar.zst` (use the actual downloaded filename).

After updating, load the new service policy and restart:

```bash
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

## Test priorities and known limits

- On Fedora, launch Gamescope/Wine games, disconnect or pause, switch games, then choose Quit in the Hermes tray. Check that game processes are cleaned up, no timeout-induced abort notifications appear, and Hermes stays stopped.
- Report whether the separate black-screen-on-exit behavior from Assassin's Creed Origins persists. The process cleanup change does not establish its cause or claim to fix it.
- Recheck connection/reconnection, controllers, isolated session audio/input and microphone passthrough.
- A process group cannot own descendants that deliberately move to another group/session; systemd still handles final service-cgroup cleanup.

Hardware confirmation of issue #75 remains pending. Passing CI is not proof that this Fedora/Gamescope/Wine reproduction is resolved on the reporter's machine.

## Companion versions

Hestia desktop RC1 and Hermes-KMS RC1 remain compatible. Hestia Android RC2 is prepared separately to include TV navigation, LAN/IPv6 discovery improvements and PyroWave in the same APK. Its publication requires persistent signing; use the published Android RC1 until RC2 becomes available.

PyroWave remains an explicit SDR option for fast local networks. Use the existing codecs for HDR. Experimental NVIDIA zero-copy work is outside this candidate.

## Build verification

The release is published only after the host suite, native packages and Fedora 43/44 + Ubuntu 24.04/26.04 secure-loader checks pass. Installed RPM/DEB startup is tested as a regular user with capabilities enabled. `RC_MANIFEST.json` records the exact source/workflow and payload hashes; `SHA256SUMS` covers the attached files.

## Reports and support

Include the RC/build version, distribution, GPU/driver/kernel, client, codec and logs from both ends.

Support is also available on our new [Discord server](https://discord.gg/M5dYFuhjqP). Responses may take time because the server is new, the community is small and time zones differ.
