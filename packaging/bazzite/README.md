# Hermes on Bazzite and other bootc desktops

This directory builds a bootable Bazzite-derived image with both pieces that
cannot be installed reliably after boot:

- Hermes as a native application and systemd user service;
- Hermes-KMS compiled for the exact kernel contained in the base image.

The result uses the existing KDE/GNOME/Gamescope session. It is separate from
`packaging/container`, which starts an additional headless Sway session.

## Before building

Keep the same Bazzite flavour that is already installed. Check it with:

```bash
bootc status
```

Examples include `bazzite`, `bazzite-deck`, `bazzite-nvidia` and
`bazzite-deck-nvidia`. Do not use this procedure to change desktop environment.

The local helper requires `podman`, `curl` and `jq`. It downloads the matching
Fedora RPM from the selected Hermes release unless
`packaging/bazzite/hermes.rpm` already exists.

## Local test build

Run the build as root so the result lands in root's container storage, where
`bootc switch --transport containers-storage` can read it:

```bash
sudo --preserve-env=PATH \
  BAZZITE_IMAGE=ghcr.io/ublue-os/bazzite \
  BAZZITE_TAG=stable \
  FEDORA_VERSION=44 \
  IMAGE_NAME=localhost/hermes-bazzite:latest \
  packaging/bazzite/build-image.sh

sudo bootc switch --transport containers-storage \
  localhost/hermes-bazzite:latest
sudo reboot
```

Set `BAZZITE_IMAGE` to the exact installed flavour. `HERMES_RELEASE=nightly`
selects the rolling prerelease; the default is the latest stable release.

After reboot:

```bash
hermes-bazzite-setup enable
hermes-bazzite-doctor
```

Only one of Hermes, Apollo and Sunshine can run at once. The setup helper stops
and disables the two conflicting user services before enabling Hermes. It does
not uninstall either application.

If firewalld is active, allow LAN clients once:

```bash
sudo firewall-cmd --permanent --add-service=hermes
sudo firewall-cmd --reload
```

## Secure Boot

An enforcing Secure Boot kernel refuses an unsigned out-of-tree module. Image
signing with Cosign and kernel-module signing are separate operations.

For a personal image, create and enroll a MOK, then provide its private key and
certificate only as build secrets:

```bash
openssl req -new -x509 -newkey rsa:4096 \
  -keyout MOK.priv -outform DER -out MOK.der \
  -nodes -days 3650 -subj '/CN=Hermes-KMS local module/'

sudo mokutil --import MOK.der
# Reboot once and complete enrollment in MokManager.

sudo --preserve-env=PATH,MODULE_SIGN_KEY,MODULE_SIGN_CERT \
  MODULE_SIGN_KEY="$PWD/MOK.priv" \
  MODULE_SIGN_CERT="$PWD/MOK.der" \
  packaging/bazzite/build-image.sh
```

The private key is mounted only in the discarded build stage. The public
certificate is installed as
`/usr/share/hermes-kms/module-signing-certificate.der`, making it possible to
audit or enroll the key used by a published image. The helper forces a no-cache
build when signing so key rotation cannot reuse an old module layer. Without
these variables the build succeeds but prints a warning; Secure Boot must then
be disabled for `hermes_kms` to load.

Long term, Hermes-KMS should be packaged as an akmod in `ublue-os/akmods` so
Universal Blue's existing build and signing pipeline can produce a module for
each Bazzite kernel.

## Isolated-session prototype

The normal image installs one host card for the logged-in compositor. To include
the optional driver seat broker and its runtime dependencies:

```bash
sudo --preserve-env=PATH \
  WITH_ISOLATED_SESSIONS=1 \
  packaging/bazzite/build-image.sh
```

This only installs the prerequisites. Hermes' isolated-session mode remains
experimental and should not be the first validation target.

## Updates

A local image does not magically follow its remote Bazzite base. Re-run the
build periodically, then run:

```bash
sudo bootc upgrade
sudo reboot
```

For unattended updates, publish the image to a registry. The included GitHub
workflow rebuilds after successful Hermes releases and on a daily schedule so a
new Bazzite kernel always gets a matching Hermes-KMS module.

## Rollback and removal

If the new deployment fails, select the previous deployment in the boot menu or
run:

```bash
sudo bootc rollback
sudo reboot
```

After returning to the original Bazzite image, remove the local test image only
if it is no longer referenced by a deployment:

```bash
sudo podman image rm localhost/hermes-bazzite:latest
```

User configuration remains under `~/.config/hermes`.

## Validation checklist

`hermes-bazzite-doctor` checks the common failure points. The equivalent manual
checks are:

```bash
uname -r
modinfo -k "$(uname -r)" hermes_kms
lsmod | grep hermes_kms
ls /dev/dri/by-path/ | grep hermes
getcap /usr/bin/hermes
systemctl --user status hermes
journalctl --user -u hermes -b
```

The image build itself verifies that the RPM matches the Fedora release, the
module matches the image kernel, `depmod` can index it, the expected integration
files exist, and `bootc container lint` accepts the final filesystem.
