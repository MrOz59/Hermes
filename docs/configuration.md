---
mdx:
  format: mdx
---

import ConfigurationHost from './site/src/components/ConfigurationHost';

# Configuration

<ConfigurationHost />

Hermes will work with the default settings for most users. In some cases you may want to configure Hermes further.

The default location for the configuration file is listed below. You can use another location if you
choose, by passing in the full configuration file path as the first argument when you start Hermes.

**Example**
```bash
hermes ~/hermes.conf
```

The default location of the `apps.json` is the same as the configuration file. You can use a custom
location by modifying the configuration file.

**Default Config Directory**

| OS      | Location                                        |
|---------|-------------------------------------------------|
| Docker  | `/config`                          |
| Linux   | `~/.config/hermes`               |

Use the configuration UI or edit the `conf` file in a text editor. Use the examples as reference.

## General

### locale

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The locale used for Hermes's user interface.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`en`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`locale = en`</td>
    </tr>
    <tr>
        <td rowSpan={20}>Choices</td>
        <td>bg</td>
        <td>Bulgarian</td>
    </tr>
    <tr>
        <td>cs</td>
        <td>Czech</td>
    </tr>
    <tr>
        <td>de</td>
        <td>German</td>
    </tr>
    <tr>
        <td>en</td>
        <td>English</td>
    </tr>
    <tr>
        <td>en_GB</td>
        <td>English (UK)</td>
    </tr>
    <tr>
        <td>en_US</td>
        <td>English (United States)</td>
    </tr>
    <tr>
        <td>es</td>
        <td>Spanish</td>
    </tr>
    <tr>
        <td>fr</td>
        <td>French</td>
    </tr>
    <tr>
        <td>it</td>
        <td>Italian</td>
    </tr>
    <tr>
        <td>ja</td>
        <td>Japanese</td>
    </tr>
    <tr>
        <td>ko</td>
        <td>Korean</td>
    </tr>
    <tr>
        <td>pl</td>
        <td>Polish</td>
    </tr>
    <tr>
        <td>pt</td>
        <td>Portuguese</td>
    </tr>
    <tr>
        <td>pt_BR</td>
        <td>Portuguese (Brazilian)</td>
    </tr>
    <tr>
        <td>ru</td>
        <td>Russian</td>
    </tr>
    <tr>
        <td>sv</td>
        <td>Swedish</td>
    </tr>
    <tr>
        <td>tr</td>
        <td>Turkish</td>
    </tr>
    <tr>
        <td>uk</td>
        <td>Ukranian</td>
    </tr>
    <tr>
        <td>zh</td>
        <td>Chinese (Simplified)</td>
    </tr>
    <tr>
        <td>zh_TW</td>
        <td>Chinese (Traditional)</td>
    </tr>
  </tbody>
</table>

### sunshine_name

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The name shown by Hestia and other compatible clients.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>PC hostname</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`sunshine_name = Hermes`</td>
    </tr>
  </tbody>
</table>

### min_log_level

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The minimum log level printed to standard out.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`info`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`min_log_level = info`</td>
    </tr>
    <tr>
        <td rowSpan={7}>Choices</td>
        <td>verbose</td>
        <td>

All logging message.

:::warning

This may negatively affect streaming performance.

:::

    </td>
    </tr>
    <tr>
        <td>debug</td>
        <td>

Debug log messages and higher.

:::warning

This may negatively affect streaming performance.

:::

    </td>
    </tr>
    <tr>
        <td>info</td>
        <td>Informational log messages and higher.</td>
    </tr>
    <tr>
        <td>warning</td>
        <td>Warning log messages and higher.</td>
    </tr>
    <tr>
        <td>error</td>
        <td>Error log messages and higher.</td>
    </tr>
    <tr>
        <td>fatal</td>
        <td>Only fatal log messages.</td>
    </tr>
    <tr>
        <td>none</td>
        <td>No log messages.</td>
    </tr>
  </tbody>
</table>

### global_prep_cmd

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

A list of commands to be run before/after all applications.
If any of the prep-commands fail, starting the application is aborted.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`[]`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`global_prep_cmd = [{"do":"xrandr --output HDMI-1 --mode 1920x1080","undo":"xrandr --output HDMI-1 --mode 3840x2160"}]`</td>
    </tr>
  </tbody>
</table>

### global_state_cmd

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Commands run on host state transitions (e.g. session start/stop), analogous to global_prep_cmd but tied to host state changes.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`[]`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`global_state_cmd = [{"do":"","undo":""}]`</td>
    </tr>
  </tbody>
</table>

### server_cmd

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Named commands a client may run on the host during a session —
"Server Commands" in the Web UI. They are not run by Hermes on its
own: the names are advertised to a paired client that holds the
<code>server_cmd</code> permission, which then asks for one by name.
A client without that permission is not told they exist.

:::note

Also reachable over the Hestia API as
<code>GET /api/hestia/v1/commands</code> and
<code>POST /api/hestia/v1/commands/run</code>, with the same
permission.

:::

:::warning

A server command runs with the privileges of the Hermes
process, chosen by whoever configures the host and triggered by a
remote client. Grant the permission only to clients you would trust
with those commands.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`[]`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`server_cmd = [{"name":"Open Steam","cmd":"steam"}]`</td>
    </tr>
  </tbody>
</table>

### notify_pre_releases

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Whether the Web UI should check the rolling Hermes nightly channel
in addition to stable releases. Disabled by default because nightly
builds may be unstable. This only displays an update notification
and download link; Hermes does not install updates automatically.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`notify_pre_releases = disabled`</td>
    </tr>
  </tbody>
</table>

### announcements

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Whether the Web UI home page shows announcements published by the
Hermes maintainers, such as a known issue affecting your version.
The message list is fetched from GitHub when the home page loads,
so disabling this stops that request. Announcements never install
or change anything; they are text, an optional link, and a dismiss
button.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`announcements = enabled`</td>
    </tr>
  </tbody>
</table>

### system_tray

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Show icon in system tray and display desktop notifications</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`system_tray = enabled`</td>
    </tr>
  </tbody>
</table>

### hide_tray_controls

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Hide interactive controls in the system-tray menu, leaving only status entries.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`hide_tray_controls = enabled`</td>
    </tr>
  </tbody>
</table>

### enable_pairing

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Allow new clients to pair with the host. Disable to lock the host to already-paired clients.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`enable_pairing = disabled`</td>
    </tr>
  </tbody>
</table>

### enable_discovery

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Advertise the host for discovery (mDNS) so clients can find it automatically on the network.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`enable_discovery = disabled`</td>
    </tr>
  </tbody>
</table>

### inhibit_screen_lock

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Keep the host's screensaver and screen lock from engaging while a
client is connected, by holding an inhibition through
<code>org.freedesktop.ScreenSaver</code> on the D-Bus session bus.
The inhibition is released when the last client disconnects.

:::note

This option applies to Linux only. It needs a desktop
screensaver service implementing <code>org.freedesktop.ScreenSaver</code>
on the same D-Bus user bus as Hermes. The desktop must honor the
request; a manual lock or administrator-enforced lock is not
overridden. If the service restarts, Hermes attempts to restore
the inhibition. Use Save and Apply in the web UI (which restarts
Hermes) after changing this setting; saving alone does not change
an already-running stream.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`inhibit_screen_lock = enabled`</td>
    </tr>
  </tbody>
</table>

## Input

### controller

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Whether to allow controller input from the client.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`controller = enabled`</td>
    </tr>
  </tbody>
</table>

### gamepad

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The type of gamepad to emulate on the host. On Linux, auto uses the client-reported
controller type (PlayStation to DualSense, Nintendo to Switch Pro, Xbox to Xbox Series).
Unknown types can select DualSense from motion/touchpad capabilities; otherwise Xbox
Series is used. A manual selection overrides client detection.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`auto`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`gamepad = auto`</td>
    </tr>
    <tr>
        <td rowSpan={7}>Choices</td>
        <td>generic</td>
        <td>

Generic HID gamepad

:::note

This option applies to Linux only.

:::

    </td>
    </tr>
    <tr>
        <td>ds4</td>
        <td>DualShock 4 controller (PS4)</td>
    </tr>
    <tr>
        <td>ds5</td>
        <td>

DualSense controller (PS5)

:::note

This option applies to Linux only.

:::

    </td>
    </tr>
    <tr>
        <td>switch</td>
        <td>

Switch Pro controller

:::note

This option applies to Linux only.

:::

    </td>
    </tr>
    <tr>
        <td>x360</td>
        <td>Xbox 360 controller</td>
    </tr>
    <tr>
        <td>xone</td>
        <td>

Xbox One controller

:::note

This option applies to Linux only.

:::

    </td>
    </tr>
    <tr>
        <td>xseries</td>
        <td>

Xbox Series controller

:::note

This option applies to Linux only.

:::

    </td>
    </tr>
  </tbody>
</table>

### linux_gamepad_backend

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Linux controller creation policy. Auto preserves ordinary evdev rumble
without hidraw access for Xbox controllers and uses native HID for PlayStation/Switch.
Native exposes advanced capabilities, but consuming games need access to the generated
hidraw device. Uinput restricts automatic selection to Xbox Series; manually selected
PlayStation/Switch profiles require auto or native. This option does not change other platforms.

    </td>
    </tr>
    <tr><td>Default</td><td colSpan={2}>`auto`</td></tr>
    <tr><td>Example</td><td colSpan={2}>`linux_gamepad_backend = auto`</td></tr>
    <tr><td rowSpan={3}>Choices</td><td>auto</td><td>Input events for Xbox; native HID for PlayStation/Switch</td></tr>
    <tr><td>native</td><td>Prefer native HID and advanced features</td></tr>
    <tr><td>uinput</td><td>Input-event controllers only, without hidraw requirements</td></tr>
  </tbody>
</table>

### ds4_back_as_touchpad_click

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Allow Select/Back inputs to also trigger a PlayStation touchpad click. Useful for clients looking to
emulate touchpad click on Xinput devices.

:::tip

Applies to DS4, DS5 and automatically selected PlayStation profiles.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`ds4_back_as_touchpad_click = enabled`</td>
    </tr>
  </tbody>
</table>

### motion_as_ds4

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

If a client reports that a connected gamepad has motion sensor support, emulate it on the
host as a PlayStation-style controller (DualSense).

When disabled, motion sensors will not be taken into account during gamepad type selection.

:::tip

Only applies when gamepad is set to auto.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`motion_as_ds4 = enabled`</td>
    </tr>
  </tbody>
</table>

### touchpad_as_ds4

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

If a client reports that a connected gamepad has a touchpad, emulate it on the host
as a PlayStation-style controller (DualSense).

When disabled, touchpad presence will not be taken into account during gamepad type selection.

:::tip

Only applies when gamepad is set to auto.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`touchpad_as_ds4 = enabled`</td>
    </tr>
  </tbody>
</table>

### virtualhid_randomize_mac

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Randomize the MAC address for virtual DualShock 4 and DualSense controllers on Linux.
When disabled, deterministic identities remain scoped to the isolated session.
Older ds5_virtualhid_randomize_mac and ds5_inputtino_randomize_mac keys are accepted;
an explicitly supplied virtualhid_randomize_mac value takes precedence.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`virtualhid_randomize_mac = enabled`</td>
    </tr>
  </tbody>
</table>

The former `ds5_inputtino_randomize_mac` key is accepted as a compatibility
alias; new configurations should use the key above.

### virtualhid_stable_mac

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Give a virtual Switch Pro, DualShock 4 or DualSense controller on Linux a MAC address derived from the
paired client and its controller number, the same in every session, seat and reconnection. Steam keeps
per-controller layouts by that address (the controller's serial), so they stay applied. When enabled it
takes precedence over virtualhid_randomize_mac; when disabled, the Switch Pro address is scoped to the
session and PlayStation addresses follow virtualhid_randomize_mac.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`virtualhid_stable_mac = disabled`</td>
    </tr>
  </tbody>
</table>

### back_button_timeout

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

If the Back/Select button is held down for the specified number of milliseconds,
a Home/Guide button press is emulated.

:::tip

If back_button_timeout &lt; 0, then the Home/Guide button will not be emulated.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`-1`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`back_button_timeout = 2000`</td>
    </tr>
  </tbody>
</table>

### keyboard

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Whether to allow keyboard input from the client.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`keyboard = enabled`</td>
    </tr>
  </tbody>
</table>

### key_repeat_delay

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The initial delay, in milliseconds, before repeating keys. Controls how fast keys will
repeat themselves.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`500`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`key_repeat_delay = 500`</td>
    </tr>
  </tbody>
</table>

### key_repeat_frequency

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

How often keys repeat every second.

:::tip

This configurable option supports decimals.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`24.9`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`key_repeat_frequency = 24.9`</td>
    </tr>
  </tbody>
</table>

### key_rightalt_to_key_win

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

It may be possible that you cannot send the Windows key from your client directly. In those cases it may be useful to
map the Right Alt key to the Super key on the host.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`key_rightalt_to_key_win = enabled`</td>
    </tr>
  </tbody>
</table>

### mouse

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Whether to allow mouse input from the client.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`mouse = enabled`</td>
    </tr>
  </tbody>
</table>

### high_resolution_scrolling

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

When enabled, Hermes will pass through high-resolution scroll events from clients.
<br />
This can be useful to disable for older applications that scroll too fast with high resolution scroll
events.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`high_resolution_scrolling = enabled`</td>
    </tr>
  </tbody>
</table>

### native_pen_touch

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

When enabled, Hermes will pass through native pen/touch events from clients.
<br />
This can be useful to disable for older applications without native pen/touch support.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`native_pen_touch = enabled`</td>
    </tr>
  </tbody>
</table>

### isolated_ignore_mouse_wiggle

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Moonlight-based clients move the mouse by one pixel and back right after connecting to wake the host
display. In an isolated gamescope session that is real motion and shows the cursor until the app hides it.
When enabled, Hermes drops that +1,+1 / -1,-1 move if it is the first mouse input of an isolated session.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`isolated_ignore_mouse_wiggle = disabled`</td>
    </tr>
  </tbody>
</table>

### isolated_hide_hid_gamepads

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Switch Pro, DualShock 4 and DualSense emulation are HID devices that a kernel driver (`hid-nintendo`,
`hid-playstation`) turns into a gamepad. Steam reads them through hidraw and does not grab that gamepad, so a
game that reads it too gets every press twice: raw, and through Steam Input. When enabled, an isolated
session (Gamescope or KWin virtual backend) gets only their hidraw nodes, and games see Steam Input's controller
alone. A game run without Steam Input then has no controller.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`isolated_hide_hid_gamepads = enabled`</td>
    </tr>
  </tbody>
</table>

### enable_input_only_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Allow input-only sessions, where a client sends input without receiving a video/audio stream.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`enable_input_only_mode = disabled`</td>
    </tr>
  </tbody>
</table>

### forward_rumble

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Forward controller rumble/force-feedback events from the host back to the client gamepad.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`forward_rumble = disabled`</td>
    </tr>
  </tbody>
</table>

### keybindings

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Sometimes it may be useful to map keybindings. Wayland won't allow clients to capture the Win Key
for example.

:::tip

See [virtual key codes](https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)

:::

:::tip

keybindings needs to have a multiple of two elements.

:::

:::note

This option is not available in the UI. A PR would be welcome.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>

```
[
  0x10, 0xA0,
  0x11, 0xA2,
  0x12, 0xA4
]
```

    </td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>

```
keybindings = [
  0x10, 0xA0,
  0x11, 0xA2,
  0x12, 0xA4,
  0x4A, 0x4B
]
```

    </td>
    </tr>
  </tbody>
</table>

## Audio/Video

### audio_sink

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The name of the audio sink used for audio loopback.

:::tip

To find the name of the audio sink follow these instructions.

**Linux + pulseaudio:**
<br />
`pacmd list-sinks | grep "name:"`

**Linux + pipewire:**
<br />

```
pactl info | grep Source
# in some causes you'd need to use the `Sink` device, if `Source` doesn't work, so try:
pactl info | grep Sink
```

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>Hermes will select the default audio device.</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`audio_sink = alsa_output.pci-0000_09_00.3.analog-stereo`</td>
    </tr>
  </tbody>
</table>

### stream_audio

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Whether to stream audio or not. Disabling this can be useful for streaming headless displays as second monitors.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`stream_audio = disabled`</td>
    </tr>
  </tbody>
</table>

### stream_mic

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Offer clients microphone passthrough (Apollo's protocol; Moonlight forks such as Artemis
builds that support it). The microphone reaches an independent client session only, as
the only source of that session's private audio server; it never reaches the host's
desktop. Experimental.

:::note

Applies to Linux independent client sessions (hermes_kms_isolated_sessions).

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`stream_mic = enabled`</td>
    </tr>
  </tbody>
</table>

### keep_sink_default

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Keep the captured audio sink as the system default while streaming, instead of switching the default to the streaming sink. Disable to let Hermes take over the default sink for the session.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`keep_sink_default = disabled`</td>
    </tr>
  </tbody>
</table>

### auto_capture_sink

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Automatically capture from the current default audio sink rather than a fixed device. Disable to always capture the configured audio sink.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`auto_capture_sink = disabled`</td>
    </tr>
  </tbody>
</table>

### adapter_name

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Select the video card you want to stream.

:::tip

To find the appropriate values follow these instructions.

**Linux + VA-API:**
<br />
Unlike with *amdvce* and *nvenc*, it doesn't matter if video encoding is done on a different GPU.

```
ls /dev/dri/renderD*  # to find all devices capable of VAAPI
# replace ``renderD129`` with the device from above to list the name and capabilities of the device
vainfo --display drm --device /dev/dri/renderD129 | \
  grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
```

To be supported by Hermes, it needs to have at the very minimum:
`VAProfileH264High   : VAEntrypointEncSlice`

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>Hermes will select the default video card.</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`adapter_name = /dev/dri/renderD128`</td>
    </tr>
  </tbody>
</table>

### output_name

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Select the display number you want to stream.

:::tip

To find the appropriate values follow these instructions.

**Linux:**
<br />
During Hermes startup, you should see the list of detected displays:

```
Info: Detecting displays
Info: Detected display: DVI-D-0 (id: 0) connected: false
Info: Detected display: HDMI-0 (id: 1) connected: true
Info: Detected display: DP-0 (id: 2) connected: true
Info: Detected display: DP-1 (id: 3) connected: false
Info: Detected display: DVI-D-1 (id: 4) connected: false
```

You need to use the id value inside the parenthesis, e.g. `1`.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>Hermes will select the default display.</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`output_name = 0`</td>
    </tr>
  </tbody>
</table>

### virtual_display_backend

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Selects which Linux virtual-display driver Hermes uses for new virtual-display sessions:
<code>evdi</code> (stable default), <code>hermes_kms</code> (zero-copy, experimental), or
<code>none</code>.

Several features exist only with <code>hermes_kms</code>: zero-copy capture on AMD/Intel,
HDR10 on a virtual display, <code>hermes_kms_multi_output</code>,
<code>hermes_kms_isolated_sessions</code>, streaming before login, and the driver's
capture counters in diagnostics. It needs Hermes-KMS v0.4.0 or newer; Hermes does not fall
back to EVDI when it is selected but unavailable.

<code>none</code> is for a host that has no virtual-display device and does not want one,
because the output it streams already exists - a headless compositor session is the usual
case. It also stops Hermes reporting readiness problems for a driver that host never uses.
<code>headless</code> is accepted as an older spelling of <code>none</code>.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`evdi`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`virtual_display_backend = hermes_kms`</td>
    </tr>
  </tbody>
</table>

### hermes_kms_multi_output

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Enables the experimental shared-desktop multi-output Hermes-KMS
path. Each simultaneously connected client receives a separate
virtual output and capture pipeline, but all outputs remain part of
the host compositor and desktop session. Requires Hermes-KMS UAPI 8
or newer loaded with more than one output, for example
<code>initial_enabled=0 outputs=2</code>.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`false`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>

```
virtual_display_backend = hermes_kms
hermes_kms_multi_output = true
```

    </td>
    </tr>
  </tbody>
</table>

### hermes_kms_isolated_sessions

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

:::warning

Experimental and not recommended for production yet. Independent-session support now coexists with the shared host session rather than replacing it. Resource limits, GPU compositor support, per-account audio isolation, and full Plasma-session validation remain unfinished.

:::

Enables the experimental independent-session capability. It does not switch the entire Hermes server into an isolated mode. Eligible launches may use independent sessions while ordinary Host Desktop, Remote Input, and other shared routes continue to use the existing host compositor.

<b>What every isolated session gets.</b> The Plasma
desktop (<code>hermes_kms_session_compositor = plasma</code>) runs
on a virtual KWin output instead; every
other isolated session gets its own Hermes-KMS DRM card
on its own private DRM seat, its own compositor, its own Wayland
socket, its own capture pipeline, and its own set of virtual input
devices, tagged so that one client's keyboard and mouse cannot enter
another client's desktop. Sessions isolated with Gamescope use Gamescope;
KWin virtual ones use a session compositor chosen with
<code>hermes_kms_session_compositor</code>. This half is what
separates what each client <i>sees and controls</i>, and it does not
depend on the session broker.

<b>How far that goes depends on the session broker.</b> Separating
what each client can <i>reach</i> - files, credentials, the host's
desktop services - takes a separate Unix user, which is the only
boundary the Linux kernel actually enforces. Hermes runs as an
ordinary user and cannot create accounts or become anybody, so that
half is done by <code>hermes-session-broker</code>, a small root
service, and it is optional.

<b>Without the session broker</b>, everything in the session runs
under the same uid Hermes runs under. The client's compositor and
applications therefore share the Hermes user's home directory, its
configuration, its SSH and GPG keys, its browser and game-store
profiles, and its D-Bus session bus - which also means an
application that registers as a unique instance may hand its window
to the copy already running on the host desktop rather than opening
it in the session. This mode isolates displays and input. It does
not isolate files, and is only appropriate where every client is
already trusted with the host account.

<b>With the session broker</b>, each paired client is given a Unix
account of its own - <code>hermes-s01</code> upwards, derived from a
slot and never from anything the client sent - and the session runs
as that user in a transient systemd unit. The account has a private
<code>0700</code> home under
<code>/var/lib/hermes/sessions</code>, kept across reconnections so
saves and settings survive; a runtime directory, user manager and
session bus of its own; no shell and no sudo; and membership in none
of <code>wheel</code>, <code>video</code>, <code>input</code> or
<code>render</code>. Its GPU access comes from its own card's
per-card <code>access_uid</code>. It is in the
<code>hermes-session</code> group, which the shipped polkit rule
refuses every action to - without that rule a session on a local
seat would count as "active and local" to polkit and be handed over
a hundred passwordless actions, including powering the host off and
mounting its disks. The session's log goes to the journal, under
<code>journalctl -u hermes-session-N</code>.

If the broker is installed but will not provide an account - the
Hermes user is not in its allow file, or every configured slot is in
use - the launch fails instead of falling back to the host user,
because a silent fallback is the one outcome installing it was meant
to prevent.

Requires a Hermes-KMS driver speaking UAPI 11 or newer and its
session-seat udev rule, a <code>gamescope</code> that can scan out
on another device than it renders with (<code>--prefer-drm</code>
or <code>--drm-device</code>, as in Hermes' own Gamescope build),
the chosen session compositor, and one packaged
<code>hermes-kms-seatd@N.service</code> private broker per device,
which <code>hermes-kms-setup</code> configures for the Hermes user.
The Hermes user must be a member of the <code>seat</code> group.
Hermes maps device N to
<code>/run/hermes-kms-seatd/N/seatd.sock</code>.
<code>hermes_kms_multi_output</code> may be enabled at the same time. Shared-host launches continue to use host compositor outputs, while independent launches allocate private session devices.
Audio follows the same split. A session that shares the Hermes
user gets a sink of its own, named after the session; its
applications play into it, capture records it by name, and the
host's default sink is never moved - so the client hears its own
session and not the machine. A session that has a Unix account of
its own does not yet: it gets an audio server of its own along with
its user manager, which Hermes cannot reach through a
<code>0700</code> runtime directory, so that case still hears the
host's audio and says so in the log.

:::warning

Audio is not yet isolated for sessions that have a Unix account of their own - those clients hear the host. A full Plasma desktop and simultaneous real clients are not validated. The private seat brokers are not a security boundary between mutually untrusted local users. Remote Input remains a shared-host feature. Keep independent-session support disabled outside testing.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`false`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>

```
virtual_display_backend = hermes_kms
hermes_kms_isolated_sessions = true
```

Install the driver's broker (<code>hermes-kms-seatd-git</code>, or
<code>sudo make INSTALL_BROKER=1 dkms-install</code> from a clone)
and configure the session pool as the user Hermes runs as:
`sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto`
That configures one seat0 host card plus private session cards
(four by default, <code>--session-devices N</code> for up to eight)
and starts their brokers, asking for one reboot when a different
module topology is already loaded. Joining the <code>seat</code> group and
enabling <code>hermes-kms-seatd@N.service</code> by hand is no
longer needed; the helper removes that older enablement.
A driver with UAPI 12 or newer can create cards at runtime instead,
so the pool no longer has to be sized in advance. That is what the
packaged card broker is for: with it enabled, an exhausted pool is
answered by a new card on a private seat rather than by a refusal.

```
echo hermes | sudo tee /etc/hermes/card-broker.allow
sudo systemctl enable --now hermes-kms-card-broker.socket
```

The broker runs as root because the driver's configfs group is
root's; it answers only the users listed in that file, allocates the
session index itself (the index is what becomes the private seat, so
it cannot be a client's choice), and lets a user remove only the
cards it created. Cards are removed when their display is torn down,
and any left by a Hermes that died are swept at the next start.

To give each client a Unix account of its own, enable the session
broker as well. Without this, sessions are isolated by display and
input but still run as the Hermes user:

```
echo hermes | sudo tee /etc/hermes/session-broker.allow
sudo systemctl enable --now hermes-session-broker.socket
```

Both brokers are authorized the same way and neither uses polkit:
the caller's uid comes from the kernel through
<code>SO_PEERCRED</code>, so it cannot be claimed in a request, and
it is checked against a file only root can write. The session broker
additionally refuses to create an account at all while the
<code>hermes-session</code> group is missing, so an account cannot
come to exist on a machine where the polkit rule that confines it is
not yet installed. Accounts are never removed implicitly - a client
that unpairs keeps its account and its home, because a save game
outliving an accidental unpair is worth more than a tidy passwd
file. Removing one is a deliberate act:

```
sudo systemctl stop hermes-session-broker.socket
sudo userdel --remove hermes-s01
```

    </td>
    </tr>
  </tbody>
</table>

### gamescope_backend

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Backend used when launching a desktop application through Gamescope.
<code>auto</code> nests Gamescope in the current Wayland compositor;
<code>wayland</code>, <code>sdl</code> or <code>drm</code> force a
specific backend.

:::note

A nested Gamescope is an ordinary window: the desktop
compositor chooses where it opens, normally the primary monitor, and
this setting never moves it onto the virtual display. To have it
appear in the stream, give the app a
<code>virtual-display-layout</code> of <code>mirror</code> (clone
the desktop) or <code>exclusive</code>, or enable
isolated_virtual_display_option globally.

:::

:::note

Ignored in a standalone Gamescope session (e.g. SteamOS Game
Mode), where the application runs directly in the existing session.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`auto`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`gamescope_backend = wayland`</td>
    </tr>
  </tbody>
</table>

### hermes_kms_session_compositor

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Which desktop the <code>kwin_virtual</code> isolation method starts.
The default, <code>plasma</code>, is the user's own Plasma desktop
on KWin's virtual backend (see below). Any other name replaces KWin
with a
compositor profile file, looked up first in
<code>~/.config/hermes/session-compositors/</code> and then in the
profiles Hermes ships; a profile declares the command line, any
environment it needs, and whether it is told its Wayland socket name
or names its own, and the compositor runs on a private Hermes-KMS
DRM seat.

:::note

Hermes ships <code>weston</code> and <code>labwc</code>
profiles as opt-in fallbacks for machines without Plasma 6. Neither
is required. Dropping in a profile for a third compositor needs no
code change and is supported by nobody but you.

:::

:::note

Of the two profiles, <code>weston</code> is the one to use with
Hermes-KMS. It takes KMS nodes only, for both its display and its
rendering device, and the sole KMS node backed by a real GPU belongs
to seat0 - which a session on a private seat cannot open - so a
session under weston composites in software. wlroots takes the two
separately, which is what <code>labwc</code> is for; but it
allocates an output's scanout buffers on the display device's own
render node, and Hermes-KMS's is capture-only with no Mesa driver
behind it, so every swapchain fails. Use <code>labwc</code> only
with a KMS device whose render node Mesa can drive.

:::

:::note

<code>plasma</code> is not a profile file. It starts the
user's own Plasma desktop: stock KWin on the virtual output of its
virtual backend, run as the same user in a transient unit of the user's
systemd manager, with a private D-Bus and runtime directory. It
shares the user's home, files and settings, so changes made in it
are real; it has no DRM device and no libinput, so it never wakes or
rearranges a physical monitor and the keyboard and mouse at the
machine never reach it. Hermes captures it through KWin's screencast
protocol and drives its input through KWin's fake-input protocol.
Its frames are copied through system memory rather than exported as
DMA-BUFs. It needs KWin and Plasma 6, plasma-wayland-protocols at
build time, and <code>dbus-run-session</code>. PowerDevil does not
run inside it, so the host's alone manages the monitors and sleep.
It runs the app's command, if any, inside the desktop.
If it cannot start, the error names the reason and suggests
<code>weston</code> as a fallback.

:::

:::note

The <code>labwc</code> profile sets the mode the client asked
for with <code>wlr-randr</code>, which is a separate package. It is
best-effort: without it the session still starts, but keeps whatever
mode wlroots preferred. Hermes-KMS 0.4.0 and later mark only the
requested mode preferred, so that is normally the right one; drivers
up to 0.3.2 marked two and the larger won.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`plasma`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`hermes_kms_session_compositor = weston`</td>
    </tr>
  </tbody>
</table>

### isolation_method

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

How a session is isolated when a client requests a virtual display,
with <code>hermes_kms_isolated_sessions</code> enabled. An app's
<code>isolation-method</code> overrides it.
<ul>
    <li><code>gamescope</code>: Gamescope with a virtual display. The
    app runs alone on its own Hermes-KMS card and private DRM seat. An
    app with no command gets the <code>kwin_virtual</code> desktop.</li>
    <li><code>kwin_virtual</code>: KWin's virtual backend, a virtual
    output with input through KWin's fake-input protocol, running the
    desktop chosen by <code>hermes_kms_session_compositor</code>. The
    app's command starts inside it.</li>
</ul>

:::note

An app on the <code>default</code> method with an explicit
<code>extend</code>, <code>mirror</code> or <code>exclusive</code>
<code>virtual-display-layout</code> stays on the host desktop.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`gamescope`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`isolation_method = kwin_virtual`</td>
    </tr>
  </tbody>
</table>

### fallback_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Resolution/mode to fall back to when the requested virtual-display mode cannot be applied. Empty leaves the fallback unset.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}></td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`fallback_mode = 1920x1080x60`</td>
    </tr>
  </tbody>
</table>

### default_scale_factor

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Host-side default resolution scale factor, in percent, applied to virtual-display sessions. 100 leaves the resolution unchanged. Range 20-200.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`100`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`default_scale_factor = 125`</td>
    </tr>
  </tbody>
</table>

### dd_configuration_option

Accepted for existing configurations. The disabled value still lets Hermes
remove saved legacy display state at startup. Use
[virtual_display_backend](#virtual_display_backend) for new Linux virtual
displays.

### dd_resolution_option

Legacy display resolution setting. The Linux display backend uses the
resolution requested by the client.

### dd_manual_resolution

Legacy companion setting for [dd_resolution_option](#dd_resolution_option).

### dd_refresh_rate_option

Legacy display refresh-rate setting. The Linux display backend uses the
refresh rate requested by the client.

### dd_manual_refresh_rate

Legacy companion setting for [dd_refresh_rate_option](#dd_refresh_rate_option).

### dd_hdr_option

Legacy display HDR toggle. See [Hermes-KMS HDR10](hermes_kms_hdr.md) for
capture requirements.

### dd_wa_hdr_toggle_delay

Legacy delay for the display HDR toggle.

### dd_config_revert_delay

Legacy display reconfiguration retry delay.

### dd_config_revert_on_disconnect

Controls when the stream requests display-configuration reversion. The default
is `disabled`. Linux compositor layout handling is described under
[isolated_virtual_display_option](#isolated_virtual_display_option).

### dd_mode_remapping

Legacy resolution and refresh-rate remapping rules.

### headless_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Run without requiring a physical display attached to the host. Useful for dedicated/headless streaming hosts.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`headless_mode = enabled`</td>
    </tr>
  </tbody>
</table>

### double_refreshrate

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Double the streamed refresh rate where the client and pipeline support it.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`double_refreshrate = enabled`</td>
    </tr>
  </tbody>
</table>

### max_bitrate

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The maximum bitrate (in Kbps) at which Hermes encodes the stream. Set it to 0 to use the bitrate requested by the client.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`0`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`max_bitrate = 5000`</td>
    </tr>
  </tbody>
</table>

### minimum_fps_target

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Hermes tries to save bandwidth when content on screen is static or a low framerate. Because many clients expect a constant stream of video frames, a certain amount of duplicate frames are sent when this happens. This setting controls the lowest effective framerate a stream can reach.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`0`</td>
    </tr>
    <tr>
        <td rowSpan={3}>Choices</td>
        <td>0</td>
        <td>Use half the stream's FPS as the minimum target.</td>
    </tr>
    <tr>
        <td>1-1000</td>
        <td>Specify your own value. The real minimum may differ from this value.</td>
    </tr>
  </tbody>
</table>

### isolated_virtual_display_option

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Isolates the virtual display, so the streamed session is the only
active output for its duration.

:::note

Individual apps can override this through their
<code>virtual-display-layout</code> field in apps.json:
<code>exclusive</code> forces this behaviour for that app, while
<code>extend</code>, <code>primary</code> or <code>mirror</code> keep the physical
outputs on even when this option is enabled. <code>primary</code>
is <code>extend</code> with the virtual display as the primary
output for the session, so applications and games open in the
stream; on KDE/KWin only, elsewhere it behaves like
<code>extend</code>.

:::

:::note

On Linux, this requires a compositor Hermes can drive:
KDE/KWin through KScreen, a Wayland compositor implementing the
wlr-output-management protocol, or GNOME through Mutter's own
DisplayConfig interface. On a compositor that offers none of them,
exclusive mode logs a warning and the physical outputs stay on.

:::

:::note

On GNOME the layout is applied as <i>temporary</i> after
Mutter's own <code>VERIFY</code> accepts it, and the layout found at
the start is submitted again when the session ends. Before the
monitors go dark Hermes also writes a physical-only layout to
<code>$XDG_STATE_HOME/hermes/saved-mutter-layout</code> and replays
it at the next start, so a crash mid-session cannot leave a desktop
with every screen off.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>enabled</td>
        <td>

Disable the physical outputs during the session and restore the
previous layout when it ends. Open windows, including a running
fullscreen game, move onto the virtual display.

    </td>
    </tr>
  </tbody>
</table>

## Network

### upnp

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Hermes will attempt to open ports for streaming over the internet.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`upnp = enabled`</td>
    </tr>
  </tbody>
</table>

When enabled, UPnP also attempts forwarding through two routers (double NAT). Hermes selects the discovered
IGD closest to the host's IPv4 address. If that router has a private WAN address and a suitable outer IGD is
also discoverable, it maps the same ports there to the inner router's WAN address. Both hops follow `upnp`;
there is no separate double-NAT switch. The application default remains disabled. Container installations
seed `upnp = enabled` only when creating a configuration without that key.

This can avoid manual forwarding on both a personal router and an ISP router, allowing direct remote
streaming without a VPN relay. Both routers must offer reachable UPnP services. It does not bypass ISP
CGNAT; if the outer IGD cannot be discovered, manual forwarding or another connectivity method is needed.
Router selection uses an IPv4 prefix heuristic, not a complete routing-table topology discovery. When two outer IGDs are equally plausible, Hermes skips the external mapping instead of arbitrarily choosing one.

Mappings to other destinations are left untouched. Existing manual mappings to the correct destination
are reused without renewal or removal, even if their description resembles a Hermes-created rule.
Hermes manages a mapping only after creating it successfully or recovering its journal record. Routers
that rewrite descriptions may prevent automatic cleanup: ambiguous permanent mappings remain in the
journal for inspection rather than being assumed removed. Hermes tracks its own rules separately for
each router and checks ownership before cleanup. Routers supporting only permanent leases are used only after recovery intent
has been written to `upnp_mappings.journal` in the configuration directory. On the next start Hermes
attempts cleanup even if UPnP was disabled; unreachable rules remain recorded for retry.

Some ISP routers report internal port `0` in a specific mapping query. For a locally recorded rule only,
Hermes corroborates the port and destination using a bounded generic enumeration, then repeats the
specific query to verify the exact description and destination before renewal or removal. Generic
descriptions are not used as ownership evidence because these routers may append stale data to them.
Incomplete enumeration, duplicate wildcard entries, changed destinations, or inconsistent leases leave
the rule untouched. This compatibility check never adopts a pre-existing mapping.

UPnP makes the configured streaming ports reachable from outside the LAN. The existing
`origin_web_ui_allowed` policy still controls whether the Web UI port is mapped. Keep MiniUPnPc updated
through your distribution to receive the XML bounds fixes for CVE-2026-110655 and CVE-2026-110656.
The fixes are in upstream commits `3cfb4fb78d5ac04ed0dadc8dd842fc9e448916db` and
`64ef08a12aabab57be0bf7e40f047c8f14dedf9c`, also backported by ProsperoSunshine.

### address_family

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Set the address family that Hermes will use.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`ipv4`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`address_family = both`</td>
    </tr>
    <tr>
        <td rowSpan={2}>Choices</td>
        <td>ipv4</td>
        <td>IPv4 only</td>
    </tr>
    <tr>
        <td>both</td>
        <td>IPv4+IPv6</td>
    </tr>
  </tbody>
</table>

### port

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Set the family of ports used by Hermes.
Changing this value will offset other ports as shown in config UI.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`47989`</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colSpan={2}>1029-65514</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`port = 47989`</td>
    </tr>
  </tbody>
</table>

### origin_web_ui_allowed

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The origin of the remote endpoint address that is not denied for HTTPS Web UI.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`lan`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`origin_web_ui_allowed = lan`</td>
    </tr>
    <tr>
        <td rowSpan={3}>Choices</td>
        <td>pc</td>
        <td>Only localhost may access the web ui</td>
    </tr>
    <tr>
        <td>lan</td>
        <td>Only LAN devices may access the web ui</td>
    </tr>
    <tr>
        <td>wan</td>
        <td>Anyone may access the web ui</td>
    </tr>
  </tbody>
</table>

### external_ip

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>If no external IP address is given, Hermes will attempt to automatically detect external ip-address.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>Automatic</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`external_ip = 123.456.789.12`</td>
    </tr>
  </tbody>
</table>

### lan_encryption_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

This determines when encryption will be used when streaming over your local network.

:::warning

Encryption can reduce streaming performance, particularly on less powerful hosts and clients.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`0`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`lan_encryption_mode = 0`</td>
    </tr>
    <tr>
        <td rowSpan={3}>Choices</td>
        <td>0</td>
        <td>encryption will not be used</td>
    </tr>
    <tr>
        <td>1</td>
        <td>encryption will be used if the client supports it</td>
    </tr>
    <tr>
        <td>2</td>
        <td>encryption is mandatory and unencrypted connections are rejected</td>
    </tr>
  </tbody>
</table>

### wan_encryption_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

This determines when encryption will be used when streaming over the Internet.

:::warning

Encryption can reduce streaming performance, particularly on less powerful hosts and clients.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`1`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`wan_encryption_mode = 1`</td>
    </tr>
    <tr>
        <td rowSpan={3}>Choices</td>
        <td>0</td>
        <td>encryption will not be used</td>
    </tr>
    <tr>
        <td>1</td>
        <td>encryption will be used if the client supports it</td>
    </tr>
    <tr>
        <td>2</td>
        <td>encryption is mandatory and unencrypted connections are rejected</td>
    </tr>
  </tbody>
</table>

### ping_timeout

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>How long to wait, in milliseconds, for data from the client before shutting down the stream.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`10000`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`ping_timeout = 10000`</td>
    </tr>
  </tbody>
</table>

## Config Files

### file_apps

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The application configuration file path. The file contains a JSON formatted list of applications that
can be started by a client.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`apps.json`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`file_apps = apps.json`</td>
    </tr>
  </tbody>
</table>

### credentials_file

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The file where user credentials for the UI are stored.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`hermes_state.json`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`credentials_file = hermes_state.json`</td>
    </tr>
  </tbody>
</table>

### log_path

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The path where the Hermes log is stored.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`hermes.log`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`log_path = hermes.log`</td>
    </tr>
  </tbody>
</table>

### pkey

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The private key used for the web UI and client pairing. For best compatibility,
this should be an RSA-2048 private key.

:::warning

Not all Moonlight clients support ECDSA keys or RSA key lengths other than 2048 bits.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`credentials/cakey.pem`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`pkey = /dir/pkey.pem`</td>
    </tr>
  </tbody>
</table>

### cert

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The certificate used for the web UI and client pairing. For best compatibility,
this should have an RSA-2048 public key.

:::warning

Not all Moonlight clients support ECDSA keys or RSA key lengths other than 2048 bits.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`credentials/cacert.pem`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`cert = /dir/cert.pem`</td>
    </tr>
  </tbody>
</table>

### file_state

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>The file where current state of Hermes is stored.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`hermes_state.json`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`file_state = hermes_state.json`</td>
    </tr>
  </tbody>
</table>

## Advanced

### fec_percentage

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Percentage of error correcting packets per data packet in each video frame.

:::warning

Higher values can correct for more network packet loss,
but at the cost of increasing bandwidth usage.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`20`</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colSpan={2}>1-255</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`fec_percentage = 20`</td>
    </tr>
  </tbody>
</table>

### qp

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Quantization Parameter. Some devices don't support Constant Bit Rate. For those devices, QP is used instead.

:::warning

Higher value means more compression, but less quality.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`28`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`qp = 28`</td>
    </tr>
  </tbody>
</table>

### min_threads

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Minimum number of CPU threads used for encoding.

:::note

Increasing the value slightly reduces encoding efficiency, but the tradeoff is usually worth it to
gain the use of more CPU cores for encoding. The ideal value is the lowest value that can reliably encode
at your desired streaming settings on your hardware.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`2`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`min_threads = 2`</td>
    </tr>
  </tbody>
</table>

### limit_framerate

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Limit the host capture/encode framerate to the client's requested rate instead of running unbounded.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`limit_framerate = enabled`</td>
    </tr>
  </tbody>
</table>

### envvar_compatibility_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Enable a compatibility mode for how environment variables are passed to launched applications, for apps that expect the legacy behavior.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`envvar_compatibility_mode = enabled`</td>
    </tr>
  </tbody>
</table>

### legacy_ordering

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Use the legacy client/device ordering behavior for compatibility with older clients.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`legacy_ordering = enabled`</td>
    </tr>
  </tbody>
</table>

### ignore_encoder_probe_failure

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Continue startup even if encoder probing fails, instead of treating a failed probe as fatal. Useful on hosts where probing is unreliable but encoding works.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`ignore_encoder_probe_failure = enabled`</td>
    </tr>
  </tbody>
</table>

### pyrowave

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Advertise the experimental PyroWave Vulkan codec to compatible Hestia clients. The codec is intra-only,
currently supports SDR YUV 4:2:0, and is intended for high-bandwidth LAN streaming. This option is only
effective when Hermes was built with the optional `pyrowave-shared` dependency and a compatible
Vulkan device is available.

On Linux the captured image is converted to YCbCr with OpenGL and the encoder reads the result through
Vulkan. On a host that encodes with VAAPI (AMD and Intel) the capture's GPU buffers are used in place,
without a copy through system memory. On a host that encodes with NVENC, Hermes-KMS virtual displays are
captured with a CPU copy and uploaded to the GPU for the conversion. If the GPU cannot share buffers
between OpenGL and Vulkan, a capture into system memory is converted on the CPU instead, which costs
several milliseconds per frame and more than a 4K frame at 60 fps allows.

A frame larger than the stream's forward error correction can protect (about 1.2 MB with the default
`fec_percentage`) is sent without it, so bitrates above roughly 550 Mbps at 60 fps are more
sensitive to packet loss.

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`pyrowave = enabled`</td>
    </tr>
  </tbody>
</table>

### hevc_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Allows the client to request HEVC Main or HEVC Main10 video streams.

:::warning

HEVC is more CPU-intensive to encode, so enabling this may reduce performance when using software
encoding.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`0`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`hevc_mode = 2`</td>
    </tr>
    <tr>
        <td rowSpan={4}>Choices</td>
        <td>0</td>
        <td>advertise support for HEVC based on encoder capabilities (recommended)</td>
    </tr>
    <tr>
        <td>1</td>
        <td>do not advertise support for HEVC</td>
    </tr>
    <tr>
        <td>2</td>
        <td>advertise support for HEVC Main profile</td>
    </tr>
    <tr>
        <td>3</td>
        <td>advertise support for HEVC Main and Main10 (HDR) profiles</td>
    </tr>
  </tbody>
</table>

### av1_mode

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Allows the client to request AV1 Main 8-bit or 10-bit video streams.

:::warning

AV1 is more CPU-intensive to encode, so enabling this may reduce performance when using software
encoding.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`0`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`av1_mode = 2`</td>
    </tr>
    <tr>
        <td rowSpan={4}>Choices</td>
        <td>0</td>
        <td>advertise support for AV1 based on encoder capabilities (recommended)</td>
    </tr>
    <tr>
        <td>1</td>
        <td>do not advertise support for AV1</td>
    </tr>
    <tr>
        <td>2</td>
        <td>advertise support for AV1 Main 8-bit profile</td>
    </tr>
    <tr>
        <td>3</td>
        <td>advertise support for AV1 Main 8-bit and 10-bit (HDR) profiles</td>
    </tr>
  </tbody>
</table>

### capture

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Force specific screen capture method.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>

Automatic.
Hermes will use the first capture method available in the order of the table above.

    </td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`capture = kms`</td>
    </tr>
    <tr>
        <td rowSpan={4}>Choices</td>
        <td>nvfbc</td>
        <td>

Use NVIDIA Frame Buffer Capture to capture direct to GPU memory. This is usually the fastest method for
NVIDIA cards. NvFBC does not have native Wayland support and does not work with XWayland.

    </td>
    </tr>
    <tr>
        <td>wlr</td>
        <td>

Capture for wlroots based Wayland compositors via wlr-screencopy-unstable-v1. It is possible to capture
virtual displays in e.g. Hyprland using this method.

    </td>
    </tr>
    <tr>
        <td>kms</td>
        <td>DRM/KMS screen capture from the kernel. This requires that Hermes has `cap_sys_admin` capability.</td>
    </tr>
    <tr>
        <td>x11</td>
        <td>Uses XCB. This is the slowest and most CPU intensive so should be avoided if possible.</td>
    </tr>
  </tbody>
</table>

### encoder

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>Force a specific encoder.</td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>Hermes will use the first encoder that is available.</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`encoder = nvenc`</td>
    </tr>
    <tr>
        <td rowSpan={3}>Choices</td>
        <td>nvenc</td>
        <td>For NVIDIA graphics cards</td>
    </tr>
    <tr>
        <td>vaapi</td>
        <td>Use Linux VA-API (AMD, Intel)</td>
    </tr>
    <tr>
        <td>software</td>
        <td>Encoding occurs on the CPU</td>
    </tr>
  </tbody>
</table>

## NVIDIA NVENC Encoder

### nvenc_preset

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

NVENC encoder performance preset.
Higher numbers improve compression (quality at given bitrate) at the cost of increased encoding latency.
Recommended to change only when limited by network or decoder, otherwise similar effect can be accomplished
by increasing bitrate.

:::note

This option only applies when using NVENC [encoder](#encoder).

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`1`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`nvenc_preset = 1`</td>
    </tr>
    <tr>
        <td rowSpan={7}>Choices</td>
        <td>1</td>
        <td>P1 (fastest)</td>
    </tr>
    <tr>
        <td>2</td>
        <td>P2</td>
    </tr>
    <tr>
        <td>3</td>
        <td>P3</td>
    </tr>
    <tr>
        <td>4</td>
        <td>P4</td>
    </tr>
    <tr>
        <td>5</td>
        <td>P5</td>
    </tr>
    <tr>
        <td>6</td>
        <td>P6</td>
    </tr>
    <tr>
        <td>7</td>
        <td>P7 (slowest)</td>
    </tr>
  </tbody>
</table>

### nvenc_twopass

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Enable two-pass mode in NVENC encoder.
This allows to detect more motion vectors, better distribute bitrate across the frame and more strictly
adhere to bitrate limits. Disabling it is not recommended since this can lead to occasional bitrate
overshoot and subsequent packet loss.

:::note

This option only applies when using NVENC [encoder](#encoder).

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`quarter_res`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`nvenc_twopass = quarter_res`</td>
    </tr>
    <tr>
        <td rowSpan={3}>Choices</td>
        <td>disabled</td>
        <td>One pass (fastest)</td>
    </tr>
    <tr>
        <td>quarter_res</td>
        <td>Two passes, first pass at quarter resolution (faster)</td>
    </tr>
    <tr>
        <td>full_res</td>
        <td>Two passes, first pass at full resolution (slower)</td>
    </tr>
  </tbody>
</table>

### nvenc_spatial_aq

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Assign higher QP values to flat regions of the video.
Recommended to enable when streaming at lower bitrates.

:::note

This option only applies when using NVENC [encoder](#encoder).

:::

:::warning

Enabling this option may reduce performance.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`nvenc_spatial_aq = disabled`</td>
    </tr>
  </tbody>
</table>

### nvenc_vbv_increase

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Single-frame VBV/HRD percentage increase.
By default Hermes uses single-frame VBV/HRD, which means any encoded video frame size is not expected to
exceed requested bitrate divided by requested frame rate. Relaxing this restriction can be beneficial and
act as low-latency variable bitrate, but may also lead to packet loss if the network doesn't have buffer
headroom to handle bitrate spikes. Maximum accepted value is 400, which corresponds to 5x increased
encoded video frame upper size limit.

:::note

This option only applies when using NVENC [encoder](#encoder).

:::

:::warning

Can lead to network packet loss.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`0`</td>
    </tr>
    <tr>
        <td>Range</td>
        <td colSpan={2}>0-400</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`nvenc_vbv_increase = 0`</td>
    </tr>
  </tbody>
</table>

### nvenc_h264_cavlc

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Prefer CAVLC entropy coding over CABAC in H.264 when using NVENC.
CAVLC is outdated and needs around 10% more bitrate for same quality, but provides slightly faster
decoding when using software decoder.

:::note

This option only applies when using H.264 format with the
NVENC [encoder](#encoder).

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`nvenc_h264_cavlc = disabled`</td>
    </tr>
  </tbody>
</table>

### nvenc_intra_refresh

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Enable NVENC intra-refresh, spreading intra-coded data across frames to smooth bitrate spikes instead of sending full keyframes.

:::note

Only applies to the NVIDIA NVENC [encoder](#encoder).

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`nvenc_intra_refresh = enabled`</td>
    </tr>
  </tbody>
</table>

## VA-API Encoder

### vaapi_strict_rc_buffer

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Enabling this option can avoid dropped frames over the network during scene changes, but video quality may
be reduced during motion.

:::note

This option only applies for H.264 and HEVC when using VA-API [encoder](#encoder) on AMD GPUs.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`disabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`vaapi_strict_rc_buffer = enabled`</td>
    </tr>
  </tbody>
</table>

### vaapi_quality

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

FFmpeg VA-API quality (global_quality) option. -1 leaves the codec default; otherwise 0-8, where lower values mean higher quality.

:::note

Only applies when using the VA-API [encoder](#encoder).

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`-1`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`vaapi_quality = 4`</td>
    </tr>
  </tbody>
</table>

### vaapi_intra_refresh

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

Rolling intra refresh: the encoder refreshes a band of rows in every frame, the whole picture every half
second. A frame lost on the network then heals on its own, so Hermes offers reference frame invalidation
and a client that lost frames keeps decoding instead of waiting for a key frame.

:::note

Only applies when using the VA-API [encoder](#encoder), with an FFmpeg build whose VA-API encoders
have the <code>intra_refresh</code> option and a driver that refreshes rows (Mesa radeonsi). Otherwise
nothing changes.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`enabled`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`vaapi_intra_refresh = disabled`</td>
    </tr>
  </tbody>
</table>

## Software Encoder

### sw_preset

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The encoder preset to use.

:::note

This option only applies when using software [encoder](#encoder).

:::

:::note

From [FFmpeg](https://trac.ffmpeg.org/wiki/Encode/H.264#preset).

A preset is a collection of options that will provide a certain encoding speed to compression ratio. A slower
preset will provide better compression (compression is quality per filesize). This means that, for example, if
you target a certain file size or constant bit rate, you will achieve better quality with a slower preset.
Similarly, for constant quality encoding, you will simply save bitrate by choosing a slower preset.

Use the slowest preset that you have patience for.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`superfast`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`sw_preset = superfast`</td>
    </tr>
    <tr>
        <td rowSpan={9}>Choices</td>
        <td>ultrafast</td>
        <td>fastest</td>
    </tr>
    <tr>
        <td>superfast</td>
        <td></td>
    </tr>
    <tr>
        <td>veryfast</td>
        <td></td>
    </tr>
    <tr>
        <td>faster</td>
        <td></td>
    </tr>
    <tr>
        <td>fast</td>
        <td></td>
    </tr>
    <tr>
        <td>medium</td>
        <td></td>
    </tr>
    <tr>
        <td>slow</td>
        <td></td>
    </tr>
    <tr>
        <td>slower</td>
        <td></td>
    </tr>
    <tr>
        <td>veryslow</td>
        <td>slowest</td>
    </tr>
  </tbody>
</table>

### sw_tune

<table>
  <tbody>
    <tr>
        <td>Description</td>
        <td colSpan={2}>

The tuning preset to use.

:::note

This option only applies when using software [encoder](#encoder).

:::

:::note

From [FFmpeg](https://trac.ffmpeg.org/wiki/Encode/H.264#preset).

You can optionally use -tune to change settings based upon the specifics of your input.

:::

    </td>
    </tr>
    <tr>
        <td>Default</td>
        <td colSpan={2}>`zerolatency`</td>
    </tr>
    <tr>
        <td>Example</td>
        <td colSpan={2}>`sw_tune = zerolatency`</td>
    </tr>
    <tr>
        <td rowSpan={6}>Choices</td>
        <td>film</td>
        <td>use for high quality movie content; lowers deblocking</td>
    </tr>
    <tr>
        <td>animation</td>
        <td>good for cartoons; uses higher deblocking and more reference frames</td>
    </tr>
    <tr>
        <td>grain</td>
        <td>preserves the grain structure in old, grainy film material</td>
    </tr>
    <tr>
        <td>stillimage</td>
        <td>good for slideshow-like content</td>
    </tr>
    <tr>
        <td>fastdecode</td>
        <td>allows faster decoding by disabling certain filters</td>
    </tr>
    <tr>
        <td>zerolatency</td>
        <td>good for fast encoding and low-latency streaming</td>
    </tr>
  </tbody>
</table>
