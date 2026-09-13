# Docker

There are two unrelated sets of container files in this repository, and only one
of them produces something you can run.

## Running Hermes in a container

`packaging/container` holds a **runtime** image: Hermes on a headless Wayland
session (sway), with audio, XWayland and an optional Steam Big Picture session.
It exists for distributions that cannot install a package normally — Bazzite,
Silverblue, SteamOS and other image-based systems — because the userspace lives
in the container and only the kernel module has to exist on the host.

```bash
cd packaging/container
docker compose build
docker compose up -d
```

The virtual display still comes from the
[Hermes-KMS](https://github.com/MrOz59/Hermes-KMS) module on the host; for an
image-based host, build it into the image with that repository's
`packaging/bazzite/Containerfile`, because DKMS cannot work there. Without the
module the container falls back to a software backend and gives up the
zero-copy path.

The image serves **one** session, so it does not compose with
`hermes_kms_multi_output` or `hermes_kms_isolated_sessions`: several clients
means one container each.

See `packaging/container/README.md` for the details, including what the host has
to expose and where it came from.

## Build images

`docker/` at the repository root holds **build** images inherited from upstream
Sunshine — Arch, Debian trixie, Ubuntu 22.04 and 24.04, plus a CLion toolchain.
They compile the project inside a distribution container and produce no runnable
host. They are not what CI builds the released packages with: that is
`.github/workflows/build.yml`, which builds in the distribution containers named
there.

> [!NOTE]
> Hermes publishes no container images. `lizardbyte/sunshine` on Docker Hub and
> ghcr.io is upstream Sunshine, a different program.

<div class="section_buttons">

| Previous                       |                                                 Next |
|:-------------------------------|-----------------------------------------------------:|
| [Changelog](docs/changelog.md) | [Third-Party Packages](docs/third_party_packages.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
