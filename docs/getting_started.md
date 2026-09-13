# Getting Started

Hermes is a Linux game-streaming host. The recommended way to run it is one of
the packages built for each release — see [Binaries](#binaries).

> [!NOTE]
> Hermes is developed on CachyOS with KDE Plasma (Wayland) on AMD, and that is
> the only configuration continuously exercised. Other distributions,
> compositors and GPUs are supported as far as their code paths allow;
> [Compatibility](compatibility.md) records what has actually been verified,
> what shares a verified code path but has never been run, and what is known
> broken.

## Binaries

Packages are built in CI and published at
[Releases](https://github.com/MrOz59/Hermes/releases):

- a **tagged release** for each version;
- a rolling **nightly** prerelease, refreshed on every push to `main`. It
  carries the newest fixes and may be unstable.

| Distribution | Asset |
|:-------------|:------|
| Arch / CachyOS | `hermes-streaming-{version}-1-x86_64.pkg.tar.zst` |
| Ubuntu 24.04, 26.04 | `hermes_{version}_ubuntu{release}_amd64.deb` |
| Fedora 43, 44 | `hermes-{version}-1.fc{release}.x86_64.rpm` |

Match the release in the filename to the one you run. Each `.deb` and `.rpm` is
built inside that release's own container, so its dependencies are the sonames
that release ships; a package built for another release will refuse to install.

> [!IMPORTANT]
> No AppImage, Flatpak, Homebrew or macOS package is published, and no Windows
> installer is attached to a release — CI builds a Windows binary on every run,
> but only as a workflow artifact. The code for those platforms is inherited
> from upstream Sunshine and still builds (see [Building](building.md)); none of
> it is exercised here. Third-party packages of *Sunshine* are not packages of
> Hermes.

### CUDA compatibility

CUDA is used for NVENC and NVFBC on NVIDIA GPUs. Every Linux package above is
built against CUDA 12.9.1, whose minimum NVIDIA driver is 575.57.08, and covers
compute capabilities 50, 52, 53, 60, 61, 62, 70, 72, 75, 80, 86, 87, 89, 90,
100, 101, 103, 120 and 121.

> [!NOTE]
> See [CUDA GPUS](https://developer.nvidia.com/cuda-gpus) to cross-reference
> Compute Capability to your GPU. Installing the CUDA toolkit yourself is only
> necessary when building from source — a package already carries what it needs.

## Install

### Arch / CachyOS

```bash
sudo pacman -U ./hermes-streaming-*.pkg.tar.zst
```

The package is named `hermes-streaming` because the `hermes` name in the AUR
belongs to an unrelated PAM authentication project — do not install that one. It
installs `/usr/bin/hermes`, `/usr/share/hermes` and a `hermes.service` user
unit, so it can sit side by side with the `apollo` (AUR) and `sunshine`
packages. Only one of the three can *run* at a time: they all bind ports
47984/47989/47990, and whichever starts second exits, which shows up in the
browser as `Failed to fetch` on the login page.

To build the package from the source tree instead, see
[CachyOS/Arch package build](../README.md) in the overview.

Uninstall:
```bash
sudo pacman -R hermes-streaming
```

### Debian / Ubuntu

```bash
sudo apt install ./hermes_{version}_ubuntu{release}_amd64.deb
```

`apt install` on the file rather than `dpkg -i`, so dependencies are resolved.
The post-install script applies `cap_sys_admin` to the binary, which KMS capture
needs.

Uninstall:
```bash
sudo apt remove hermes
```

### Fedora

```bash
sudo dnf install ./hermes-{version}-1.fc{release}.x86_64.rpm
```

Uninstall:
```bash
sudo dnf remove hermes
```

### Build from source

See [Building](building.md). Arch and CachyOS users can build the same package
CI publishes with `makepkg -sf` from the repository root.

### Image-based distributions (Bazzite, Silverblue, SteamOS)

`/usr` is read-only there, so no package lands on the installed system.
`packaging/container` holds a runtime image that runs Hermes on a headless sway
session; only the kernel module has to exist on the host. See
`packaging/container/README.md`.

### The virtual display driver

A virtual display needs a kernel module, installed separately from Hermes:

- **Hermes-KMS** (default, zero-copy) — <https://github.com/MrOz59/Hermes-KMS>,
  installed through DKMS. The overview covers the install and the
  `initial_enabled=0` module option it needs.
- **EVDI** — the supported alternative, selected automatically when Hermes-KMS
  is unavailable. On Arch it lives in the AUR (`paru -S evdi`), not the official
  repositories, so no package depends on it.

The Audio/Video settings tab shows a live diagnostic and a step-by-step install
guide when either driver is missing.


## Initial Setup
After installation, some initial setup is required.

### Linux

#### KMS Capture

> [!WARNING]
> Capture of most Wayland-based desktop environments will fail unless this step is performed.

> [!NOTE]
> `cap_sys_admin` may as well be root, except you don't need to be root to run the program. This is necessary to
> allow Hermes to use KMS capture.

Every package applies this in its post-install script, so this step is only
needed for a binary you built yourself, or after the capability was removed.

##### Enable
```bash
sudo setcap cap_sys_admin+p $(readlink -f $(which hermes))
```

#### X11 Capture
For X11 capture to work, you may need to disable the capabilities that were set for KMS capture.

```bash
sudo setcap -r $(readlink -f $(which hermes))
```

#### Service

**Start once**
```bash
systemctl --user start hermes
```

**Start on boot**
```bash
systemctl --user enable hermes
```

**Session environment**

The service runs in your user session and takes the graphical session
environment (`DISPLAY`/`WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, the audio socket,
and the session bus) from the systemd user manager. It cannot import them
itself: a service can only read the environment it was started with, so
whatever the manager does not already hold is not reachable from inside the
unit. On most desktops the compositor publishes them at login and there is
nothing to do.

If capture, audio, or launching apps (Steam/Lutris) fails when started as a
service but works when you run `hermes` from a terminal, your desktop is
probably not exporting the session environment to systemd. Import it once for
the current session and restart the service:

```bash
systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR XDG_SESSION_TYPE DBUS_SESSION_BUS_ADDRESS
systemctl --user restart hermes
```

To make this persistent, add the same `import-environment` line to your
compositor's startup (e.g. an autostart script) so it runs at every login.

#### SteamOS and Game Mode

A machine that boots straight into Game Mode - SteamOS, or CachyOS with
`gamescope-session` - runs no desktop, so there is nowhere to open the web UI
and no keyboard to type into it. Two things make Hermes usable there.

**It starts by itself.** `gamescope-session` reaches `graphical-session.target`
like every other session type, and the unit is installed into it, so enabling
the service is enough:

```bash
systemctl --user enable --now hermes
```

Hermes reports `no window system` in that session and captures through
Hermes-KMS instead. That is expected: `gamescope-session` publishes nothing to
the systemd user manager, and the KMS backend needs none of it. Set
`capture = kms` if it is not already.

**The console handles what needs a person.** `hermes-gamemode` is a
controller-driven window for the two things that cannot be done from a client:
entering the PIN for a new device, and restarting the service. Add it to Steam
once, from Desktop Mode:

1. Switch to Desktop Mode.
2. In Steam, *Games* → *Add a Non-Steam Game to My Library* → *Browse*.
3. Pick **Hermes (Game Mode)** from the application list, or
   `/usr/bin/hermes-gamemode` directly.
4. Return to Game Mode. It is in the library under *Non-Steam*.

Launch it like a game. The D-pad moves between the buttons, `A` presses, and
`B` goes back; if your controller layout sends a mouse instead, the buttons are
sized to be hit with a thumbstick. When a device is waiting to pair, the console
says so on its own and offers a numeric keypad for the four digits - no
on-screen keyboard, and nothing to type a password into. It authenticates
through a token Hermes writes into your runtime directory at `0600`, which is
why it never asks you to log in.

The console is not required for streaming; it is only needed when somebody has
to act on the host itself.

### macOS

> [!NOTE]
> Hermes publishes no macOS package; this section describes the inherited
> upstream behaviour for anyone building it there.

The first time you start it, you will be asked to grant access to screen recording and your microphone.

Only microphones can be accessed on macOS due to system limitations. To stream system audio use
[Soundflower](https://github.com/mattingalls/Soundflower) or
[BlackHole](https://github.com/ExistentialAudio/BlackHole).

> [!NOTE]
> Command Keys are not forwarded by Moonlight. Right Option-Key is mapped to CMD-Key.

> [!CAUTION]
> Gamepads are not currently supported.

## Usage

### Basic usage
The packages install a `hermes.service` systemd **user** unit, which is the
normal way to run it:

```bash
systemctl --user enable --now hermes
```

To run it in the foreground instead — useful when reading the log of a failing
start — stop the service first, since running two instances is not advised and
the second one fails to bind its ports:

```bash
systemctl --user stop hermes
hermes
```

### Specify config file
```bash
hermes <directory of conf file>/hermes.conf
```

> [!NOTE]
> You do not need to specify a config file. If no config file is entered, the default location will be used.

> [!TIP]
> The configuration file specified will be created if it doesn't exist.

### Start Hermes over SSH (Linux/X11)
Assuming you are already logged into the host, you can use this command

```bash
ssh <user>@<ip_address> 'export DISPLAY=:0; hermes'
```

If you are logged into the host with only a tty (teletypewriter), you can use `startx` to start the X server prior to
executing Hermes. You may need to add `sleep` between `startx` and `hermes` to allow more time for the display to
be ready.

```bash
ssh <user>@<ip_address> 'startx &; export DISPLAY=:0; hermes'
```

> [!TIP]
> You could also use the `~/.bash_profile` or `~/.bashrc` files to set up the `DISPLAY` variable.

@seealso{Upstream's [Remote SSH Headless Setup](https://app.lizardbyte.dev/2023-09-14-remote-ssh-headless-sunshine-setup)
guide covers a headless streaming server without autologin or dummy plugs (X11 + NVIDIA GPUs). It was written for
Sunshine, so substitute the binary and unit names. On Wayland, Hermes-KMS replaces the dummy plug — see
[Compatibility](compatibility.md).}

### Configuration

Hermes is configured via the web ui, which is available on [https://localhost:47990](https://localhost:47990)
by default. You may replace *localhost* with your internal ip address.

> [!NOTE]
> Ignore any warning given by your browser about "insecure website". This is due to the SSL certificate
> being self-signed.

> [!CAUTION]
> If running for the first time, make sure to note the username and password that you created.

1. Add games and applications.
2. Adjust any configuration settings as needed.
3. In Moonlight, you may need to add the PC manually.
4. When Moonlight requests for you insert the pin:

   - Login to the web ui
   - Go to "PIN" in the Navbar
   - Type in your PIN and press Enter, you should get a Success Message
   - In Moonlight, select one of the Applications listed

### Arguments
To get a list of available arguments, run the following command.

```bash
hermes --help
```

### Shortcuts
All shortcuts start with `Ctrl+Alt+Shift`, just like Moonlight.

* `Ctrl+Alt+Shift+N`: Hide/Unhide the cursor (This may be useful for Remote Desktop Mode for Moonlight)
* `Ctrl+Alt+Shift+F1/F12`: Switch to different monitor for Streaming

### Application List
* Applications should be configured via the web UI
* A basic understanding of working directories and commands is required
* You can use Environment variables in place of values
* `$(HOME)` will be replaced by the value of `$HOME`
* `$$` will be replaced by `$`, e.g. `$$(HOME)` will be become `$(HOME)`
* `env` - Adds or overwrites Environment variables for the commands/applications run by Hermes.
  This can only be changed by modifying the `apps.json` file directly.

### Considerations
* On Windows, Hermes uses the Desktop Duplication API which only supports capturing from the GPU used for display.
  If you want to capture and encode on the eGPU, connect a display or HDMI dummy display dongle to it and run the games
  on that display.
* When an application is started, if there is an application already running, it will be terminated.
* If any of the prep-commands fail, starting the application is aborted.
* When the application has been shutdown, the stream shuts down as well.

  * For example, if you attempt to run `steam` as a `cmd` instead of `detached` the stream will immediately fail.
    This is due to the method in which the steam process is executed. Other applications may behave similarly.
  * This does not apply to `detached` applications.

* The "Desktop" app works the same as any other application except it has no commands. It does not start an application,
  instead it simply starts a stream. If you removed it and would like to get it back, just add a new application with
  the name "Desktop" and "desktop.png" as the image path.
* In a Flatpak build you must prepend commands with `flatpak-spawn --host`.
* If inputs (mouse, keyboard, gamepads...) aren't working after connecting, add the user running hermes to the `input` group.

### HDR Support
Streaming HDR content is officially supported on Windows hosts and experimentally supported for Linux hosts.

* General HDR support information and requirements:

  * HDR must be activated in the host OS, which may require an HDR-capable display or EDID emulator dongle
    connected to your host PC.
  * You must also enable the HDR option in your Moonlight client settings, otherwise the stream will be SDR
    (and probably overexposed if your host is HDR).
  * A good HDR experience relies on proper HDR display calibration both in the OS and in game. HDR calibration can
    differ significantly between client and host displays.
  * You may also need to tune the brightness slider or HDR calibration options in game to the different HDR brightness
    capabilities of your client's display.
  * Some GPUs video encoders can produce lower image quality or encoding performance when streaming in HDR compared
    to SDR.

Additional information:

@tabs{
  @tab{ Windows |
  - HDR streaming is supported for Intel, AMD, and NVIDIA GPUs that support encoding HEVC Main 10 or AV1 10-bit profiles.
  - We recommend calibrating the display by streaming the Windows HDR Calibration app to your client device and saving an HDR calibration profile to use while streaming.
  - Older games that use NVIDIA-specific NVAPI HDR rather than native Windows HDR support may not display properly in HDR.
  }

@tab{ Linux |
  - HDR streaming is supported for Intel and AMD GPUs that support encoding HEVC Main 10 or AV1 10-bit profiles using VAAPI.
  - The KMS capture backend is required for HDR capture. Other capture methods, like NvFBC or X11, do not support HDR.
  - You will need a desktop environment with a compositor that supports HDR rendering, such as Gamescope or KDE Plasma 6.

  @seealso{[Arch wiki on HDR Support for Linux](https://wiki.archlinux.org/title/HDR_monitor_support) and
  [Reddit Guide for HDR Support for AMD GPUs](https://www.reddit.com/r/linux_gaming/comments/10m2gyx/guide_alpha_test_hdr_on_linux)}
  }
}

### Tutorials and Guides
Tutorial videos are available [here](https://www.youtube.com/playlist?list=PLMYr5_xSeuXAbhxYHz86hA1eCDugoxXY0).

Guides are available [here](guides.md).

@admonition{Community! |
Tutorials and Guides are community generated. Want to contribute? Reach out to us on our discord server.}

<div class="section_buttons">

| Previous                 |                              Next |
|:-------------------------|----------------------------------:|
| [Overview](../README.md) | [Compatibility](compatibility.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>

