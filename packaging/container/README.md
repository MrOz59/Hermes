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
namespace. If you only want to stream the desktop, set `AUTOSTART_STEAM=false`
and you can drop those options.

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
