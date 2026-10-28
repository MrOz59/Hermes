# Hermes — October 2026 RC2

This candidate fixes Linux packaging problems found while testing RC1, particularly the startup failure reported on Fedora 44.

## What changed

- **Fedora 43/44:** fixed loading of the bundled PyroWave library when Hermes runs with its required capabilities. This addresses the `libpyrowave-shared.so.0` error and exit status 127 before startup.
- **Ubuntu 24.04/26.04:** adjusted the library location and search path to prevent the same problem.
- **Ubuntu packages:** added the dependency that provides `setcap`, ensuring the required capabilities can be applied during installation.
- Updated package versions so RC2 is recognized as an upgrade from RC1.

The capabilities needed for KMS capture and isolated sessions are preserved.

## Additional packaging checks

The release pipeline now installs each RPM and DEB and checks that Hermes starts as a regular user with capabilities enabled and without a custom library environment.

It also tests secure library loading on Fedora 43/44 and Ubuntu 24.04/26.04, alongside the existing host tests.

## Installing or updating

Download the RC2 package matching your distribution.

**Fedora 44:**

```bash
sudo dnf install ./hermes-0.6.0+*rc2.*-1.fc44.x86_64.rpm
```

For Fedora 43, select the `fc43` package instead.

**Ubuntu 24.04:**

```bash
sudo apt install ./hermes_0.6.0+*rc2.*_ubuntu24.04_amd64.deb
```

For Ubuntu 26.04, select the `ubuntu26.04` package instead.

After installation, reload and restart the user service. Resetting its failed state clears the startup limit reached by the broken RC1 package:

```bash
systemctl --user daemon-reload
systemctl --user reset-failed hermes.service
systemctl --user restart hermes.service
```

Arch/CachyOS users can install the updated `hermes-streaming` package.

## Compatible components

Continue using the existing release candidates:

- [Hestia RC1](https://github.com/MrOz59/Hestia/releases/tag/october-2026-rc1)
- [Hestia-Android RC1](https://github.com/MrOz59/Hestia-Android/releases/tag/october-2026-rc1)
- [Hermes-KMS RC1](https://github.com/MrOz59/Hermes-KMS/releases/tag/october-2026-rc1)

This Hermes packaging update does not require updating those components.

## Testing and reports

This remains a prerelease. Testing on real hardware is still needed for streaming, reconnects, controllers, stylus input, isolated Gamescope sessions, private audio and microphone passthrough.

PyroWave remains an explicit SDR option intended for fast local networks. Use the existing codecs for HDR.

When reporting a problem, include the Hermes build version, distribution, GPU/driver/kernel, client device, selected codec and logs from both ends.

## Community and support

You can also ask for help on our new [Discord server](https://discord.gg/M5dYFuhjqP).

Please allow some time for a response: the server is still new, the community is small, and time zone differences may affect availability. Thanks for your patience!
