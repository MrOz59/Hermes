# Hermes in a container

A runtime image that runs Hermes on a headless Wayland session (sway), with
audio, XWayland and an optional Steam Big Picture session. This is distinct from
`docker/` at the repository root, which holds *build* images inherited from
Sunshine and produces no runnable host.

It is the practical way to run Hermes on a distribution that cannot install it
normally — Bazzite, Silverblue, SteamOS and other image-based systems — because
the userspace lives in the container and only the kernel module has to exist on
the host.

## Origin

Adapted from [SOVLOOKUP/hermes-sunshine](https://github.com/SOVLOOKUP/hermes-sunshine),
offered for upstreaming by its author in
[#6](https://github.com/MrOz59/Hermes/issues/6). That repository carries no
license file; it is included here with the author's stated permission. If you
are redistributing this, get that permission in writing first.

Changes made on import: the config paths follow Hermes' own directory instead of
the old shared Sunshine one (with a migration for volumes created before that),
the Chinese package mirrors default to off, and the compose file builds locally
rather than pulling a published image.

## Requirements on the host

The virtual display is **not** created in the container. It comes from the
[Hermes-KMS](https://github.com/MrOz59/Hermes-KMS) kernel module, which must be
loaded on the host — see that repository's README, including
`packaging/bazzite/Containerfile` for image-based systems.

Without the module the container still runs, but falls back to the wlroots
headless (software) backend, which gives up the zero-copy capture path that is
the whole point of Hermes-KMS.

## Running it

```bash
cd packaging/container
docker compose build
docker compose up -d
```

Then open `https://<host>:47990`.

The compose file grants `SYS_ADMIN` and unconfines seccomp, AppArmor and the
locked system paths. That is a wide posture, and it is there for Steam's
pressure-vessel sandbox, which does `mount --make-rslave /` inside its own mount
namespace.

With `AUTOSTART_STEAM=false` you can drop the seccomp and AppArmor unconfinement
— the default profiles are enough for the headless sway plus screencopy path.
`SYS_ADMIN` is a longer story. The image sets file capabilities on `sway`
(`cap_sys_admin+ep`) and `hermes` (`cap_sys_admin+p`) for that same sandbox, and
the effective bit on `sway` is not advisory: `execve()` of a file whose permitted
set holds a capability the process cannot receive fails with `EPERM`, so without
the grant sway did not merely lose a privilege, it did not start. What that
looked like was a host with no display at all — no Wayland socket, every encoder
probe failing, `Unable to initialize capture method`.

The entrypoint now reconciles this at startup: when `SYS_ADMIN` is missing from
the bounding set it strips the file capabilities, since they are unreachable
anyway, and sway starts without them. Desktop streaming is unaffected. Steam is
not — pressure-vessel genuinely needs the capability, so `AUTOSTART_STEAM=true`
still requires granting it.

## Podman and Quadlet

Everything here is written against Docker; on Podman the differences are real but
small.

- **Build**: `--build-arg BASE_IMAGE=...` is no longer needed. The default is
  fully qualified, because Podman enforces short-name resolution and a
  non-interactive build cannot prompt for a registry.
- **`systempaths=unconfined` / `apparmor=unconfined`** have no Podman
  equivalent, and on a Fedora-family host they are not needed for the desktop
  path. Podman uses `mask=`/`unmask=` and SELinux labels instead.
- **Devices**: `AddDevice=` takes device nodes, not directories, so `/dev/dri`
  and `/dev/input` are bind-mounted as volumes. Rootless Podman applies no
  device cgroup rules regardless — host node permissions are what govern access.
- **SELinux**: a confined rootless container cannot bind-mount devtmpfs. Running
  this one container with `SecurityLabelDisable=true` is the pragmatic answer;
  keep `:Z` on the `/config` volume.
- **Quadlet units are generator output**, so `systemctl --user enable` fails with
  *Unit is transient or generated*. The `[Install]` section in the `.container`
  file already wires auto-start; just `start` it. Add
  `loginctl enable-linger $USER` to survive logout.

A working rootless unit, `~/.config/containers/systemd/hermes.container`:

```ini
[Unit]
Description=Hermes game streaming host

[Container]
ContainerName=hermes
Image=localhost/hermes-container:latest
Network=host
Environment=AUTOSTART_STEAM=false
AddDevice=/dev/uinput
AddDevice=/dev/uhid
Volume=%h/.local/share/hermes:/config:Z
Volume=/dev/dri:/dev/dri
Volume=/dev/input:/dev/input
SecurityLabelDisable=true
PodmanArgs=--init

[Service]
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
```

Add `AddCapability=SYS_ADMIN` to that unit if you want Steam.

### Host device permissions, rootless

Rootless containers get no device cgroup delegation, so plain file permissions
decide everything:

- `/dev/dri/renderD*` is usually world-rw, so VAAPI works with the user in no
  special group at all.
- `/dev/uinput` is normally granted to the seat user by a `uaccess` ACL, so
  virtual input works.
- `/dev/uhid` frequently is **not**: on several distributions the module is not
  loaded and the node carries no `uaccess` tag, which shows up as
  `Gamepad ds5 is disabled` and no DualSense support. Two files on the *host*
  fix it:

  ```
  /etc/modules-load.d/uhid.conf          ->  uhid
  /etc/udev/rules.d/85-hermes-uhid.rules ->  KERNEL=="uhid", TAG+="uaccess"
  ```

### Hybrid-GPU hosts

On a machine with two GPUs the render node numbers follow module load order, so
the first node is not reliably the GPU the compositor renders on. The entrypoint
detects the encode GPU and writes it to `adapter_name` in `hermes.conf`, which is
what both the capture path and VAAPI read. The boot log's `render/encode GPU
node:` line says which one it chose; if that is wrong, set `adapter_name`
yourself and it will be left alone.

### Virtual display readiness

With no `hermes_kms` card the entrypoint selects the wlroots headless backend and
writes `virtual_display_backend = none`. The sway `HEADLESS-1` output *is* the
virtual display in that deployment, so there is no EVDI to install and no
readiness warning to act on. Do not enable the per-app *Always create Virtual
Display* option here; it asks for a device this deployment does not have.

## Single session only

This image runs one compositor, one seat and one `steam` user, and both the
PipeWire socket (mode 0666) and the sway Wayland socket are opened up so that
single non-root user can reach them. That is sound for one session and wrong for
several: it is a shared namespace by construction.

So it does **not** compose with `hermes_kms_multi_output` or
`hermes_kms_isolated_sessions`. Those allocate a DRM card, a private runtime
directory, a compositor and a tagged input set *per client*, precisely so two
clients cannot see each other — and this image would put them all back on one
compositor with world-accessible sockets.

Running several clients through this image means running several containers, one
per client, each with its own `/config` volume, its own port range and its own
Hermes-KMS device (`devices=N` on the host module). Making a single container
serve isolated sessions would mean rebuilding the entrypoint around per-session
seats and runtime directories, which is a larger change than this import.
