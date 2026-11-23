# Packaging

Hermes builds its executables and Web UI with mcpp. The Linux packager reads the
result manifests in `target/` and stages the files as they will appear under
`/usr`. That staged tree is the input to the distribution package tools.

- [`linux/`](linux/) contains the shared staging command and the desktop,
  AppStream, and systemd templates. It also pulls application assets from
  [`src_assets/`](../src_assets/), including the shared Hermes icon in
  [`src_assets/common/branding/`](../src_assets/common/branding/).
- [`container/`](container/) contains a runtime image definition and its
  filesystem overlay. It installs a released Hermes package.
- [`linux/flatpak/`](linux/flatpak/) retains assets and upstream helper
  submodules from the former Flatpak packaging. The active release workflow
  does not build a Flatpak.

The Arch [`PKGBUILD`](../PKGBUILD) and [`hermes.install`](../hermes.install)
stay at the repository root, where `makepkg` expects them. The
[release workflow](../.github/workflows/build.yml) adds Debian package metadata.
Fedora CI and local builds share [`linux/rpm/build-rpm.sh`](linux/rpm/build-rpm.sh)
and [`linux/rpm/hermes.spec`](linux/rpm/hermes.spec). The script consumes the staged
install tree and writes the finished artifact to `target/dist/rpm/<arch>/`.
See [Stage a Fedora package payload](../docs/modularization/mcpp-migration.md#stage-a-fedora-package-payload)
for the local commands.
