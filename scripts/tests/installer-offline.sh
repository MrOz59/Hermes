#!/usr/bin/env bash
# Offline installer helper checks: no downloads, root commands or packages.
set -euo pipefail

script="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/install.sh"
# Load function definitions without executing the final main "$@" entrypoint.
source <(sed '$d' "$script")

workdir="$(mktemp -d -t hermes-installer-test.XXXXXX)"
trap 'rm -rf -- "$workdir"' EXIT
mkdir -p "$workdir/remote" "$workdir/downloaded"
DRY_RUN=0

# Test GitHub release selection using local URLs with realistic asset names.
filename="hermes-kms-dkms_0.4.1-1_all.deb"
printf 'representative DKMS package content\n' > "$workdir/remote/$filename"
(
  cd "$workdir/remote"
  sha256sum "$filename" > SHA256SUMS
)
assets="file://$workdir/remote/$filename
file://$workdir/remote/SHA256SUMS"
url="$(release_asset_url "$assets" '/hermes-kms-dkms_.*_all\.deb$')"
[ "$url" = "file://$workdir/remote/$filename" ]

# Replace network calls with local copies; the checksum logic remains real.
download() {
  local from="${1#file://}"
  cp -- "$from" "$2"
}
download_checked_kms "$assets" "$url" "$workdir/downloaded/$filename"
cmp "$workdir/remote/$filename" "$workdir/downloaded/$filename"

# A modified package must be rejected rather than installed.
printf 'tampered\n' > "$workdir/remote/$filename"
if (download_checked_kms "$assets" "$url" "$workdir/downloaded/$filename" >/dev/null 2>&1); then
  echo "FAIL: tampered download passed checksum verification" >&2
  exit 1
fi

# DKMS must match both the selected version AND the running kernel.
dkms() {
  printf 'hermes-kms/0.4.1, %s, x86_64: installed\n' "$(uname -r)"
}
has_kms_installed
dkms() {
  printf 'hermes-kms/0.4.0, %s, x86_64: installed\n' "$(uname -r)"
}
if has_kms_installed; then
  echo "FAIL: old installed DKMS version was treated as compatible" >&2
  exit 1
fi

# Arch release assets include a pinned source commit in their DKMS version.
distro=arch
dkms() {
  printf 'hermes-kms/0.4.1.r284.gab12cd3, %s, x86_64: installed\n' "$(uname -r)"
}
has_kms_installed
distro=ubuntu
if has_kms_installed; then
  echo "FAIL: Arch -git DKMS suffix accepted for a non-Arch package" >&2
  exit 1
fi

# Reject misleading prefixes, mismatched kernels and unfinished builds.
distro=arch
dkms() {
  printf 'hermes-kms/0.4.10, %s, x86_64: installed\n' "$(uname -r)"
}
if has_kms_installed; then
  echo "FAIL: 0.4.10 was treated as 0.4.1" >&2
  exit 1
fi
dkms() {
  printf 'hermes-kms/0.4.1, other-kernel, x86_64: installed\n'
}
if has_kms_installed; then
  echo "FAIL: DKMS on another kernel was accepted" >&2
  exit 1
fi
dkms() {
  printf 'hermes-kms/0.4.1, %s, x86_64: built\n' "$(uname -r)"
}
if has_kms_installed; then
  echo "FAIL: DKMS built but not installed was accepted" >&2
  exit 1
fi

# The installer preserves unrelated settings and creates one config backup.
(
  XDG_CONFIG_HOME="$workdir/config"
  config_changed=0
  config_backed_up=0
  conf=$(config_file)
  mkdir -p "$(dirname "$conf")"
  printf 'other_setting = retained\nvirtual_display_backend = evdi\n' > "$conf"
  set_config virtual_display_backend hermes_kms
  [ "$config_changed" = 1 ]
  grep -qx 'other_setting = retained' "$conf"
  grep -qx 'virtual_display_backend = hermes_kms' "$conf"
  set_config virtual_display_backend hermes_kms
  set_config hermes_kms_isolated_sessions enabled
  [ "$(find "$(dirname "$conf")" -maxdepth 1 -name 'hermes.conf.bak.*' | wc -l)" -eq 1 ]
  backup=$(find "$(dirname "$conf")" -maxdepth 1 -name 'hermes.conf.bak.*' -print -quit)
  grep -qx 'virtual_display_backend = evdi' "$backup"
)

# Stock Gamescope is not valid for private DRM sessions.
cat > "$workdir/gamescope-patched" <<'SCRIPT'
#!/bin/sh
printf '%s\n' 'Options: --prefer-drm device'
SCRIPT
cat > "$workdir/gamescope-stock" <<'SCRIPT'
#!/bin/sh
printf '%s\n' 'Options: --nested'
SCRIPT
chmod +x "$workdir/gamescope-patched" "$workdir/gamescope-stock"
(
  gamescope_binary() { printf '%s\n' "$workdir/gamescope-patched"; }
  verify_gamescope_binary
)
(
  gamescope_binary() { printf '%s\n' "$workdir/gamescope-stock"; }
  if verify_gamescope_binary >/dev/null 2>&1; then
    echo "FAIL: stock Gamescope was accepted for isolation" >&2
    exit 1
  fi
)

# Never fall back from a missing stable release to nightly under --yes.
# die() exits the executing shell, so the expected failure must be tested
# around the entire subshell rather than inside it.
if (
  ASSUME_YES=1
  CHANNEL=release
  distro=fedora
  distro_version=43
  release_assets() {
    if [ "$2" = nightly ]; then
      touch "$workdir/nightly-was-queried"
    fi
    return 0
  }
  install_hermes >/dev/null 2>&1
); then
  echo "FAIL: stable package failure silently installed nightly" >&2
  exit 1
fi
[ ! -e "$workdir/nightly-was-queried" ]

if declare -f install_kms | grep -q 'INSTALL_BROKER=1'; then
  echo "FAIL: core Hermes-KMS stage installs optional brokers" >&2
  exit 1
fi

# An active service cannot be restarted without permission.
(
  service_was_active=1
  DRY_RUN=0
  run() { printf '%s\n' "$*" >> "$workdir/service-commands"; }
  ask() { return 1; }
  enable_service >/dev/null
)
if grep -q 'restart hermes' "$workdir/service-commands"; then
  echo "FAIL: active service restarted without consent" >&2
  exit 1
fi
(
  service_was_active=1
  DRY_RUN=0
  run() { printf '%s\n' "$*" >> "$workdir/service-commands"; }
  ask() { return 0; }
  enable_service >/dev/null
)
grep -q 'systemctl --user restart hermes' "$workdir/service-commands"

# --yes must not install an AUR package when Hermes-KMS is declined.
(
  kms_ready=0
  ASSUME_YES=1
  distro=arch
  has_evdi_installed() { return 1; }
  run() { touch "$workdir/unexpected-evdi-install"; }
  install_evdi_if_needed >/dev/null
)
[ ! -e "$workdir/unexpected-evdi-install" ]


# Version parsing must agree with the package managers, not just GitHub tags.
(
  distro=arch
  [ "$(hermes_asset_version 'https://example.invalid/hermes-streaming-0.7.0-1-x86_64.pkg.tar.zst')" = '0.7.0-1' ]
  distro=ubuntu
  distro_version=26.04
  [ "$(hermes_asset_version 'https://example.invalid/hermes_0.7.0_ubuntu26.04_amd64.deb')" = '0.7.0' ]
  distro=fedora
  distro_version=44
  [ "$(hermes_asset_version 'https://example.invalid/hermes-0.7.0-1.fc44.x86_64.rpm')" = '0.7.0-1.fc44' ]
)

# An installed stable release must not be reinstalled on every update run.
(
  MODE=update
  distro=ubuntu
  distro_version=24.04
  CHANNEL=release
  installed_package_version() { echo 0.7.0; }
  release_assets() { echo 'https://example.invalid/hermes_0.7.0_ubuntu24.04_amd64.deb'; }
  download() { touch "$workdir/unexpected-stable-download"; }
  install_local_package() { touch "$workdir/unexpected-stable-install"; }
  update_hermes >/dev/null
)
[ ! -e "$workdir/unexpected-stable-download" ]
[ ! -e "$workdir/unexpected-stable-install" ]

# Even --yes must not downgrade a newer installed host to stable packages.
(
  distro=ubuntu
  distro_version=24.04
  CHANNEL=release
  ASSUME_YES=1
  installed_package_version() { echo 0.8.0; }
  release_assets() { echo 'https://example.invalid/hermes_0.7.0_ubuntu24.04_amd64.deb'; }
  download() { touch "$workdir/unexpected-downgrade"; }
  update_hermes >/dev/null
)
[ ! -e "$workdir/unexpected-downgrade" ]

# A newer package is installed and a record of the artifact is made.
(
  distro=ubuntu
  distro_version=24.04
  CHANNEL=release
  updated_hermes=0
  installed_package_version() { echo 0.6.0; }
  release_assets() { echo 'https://example.invalid/hermes_0.7.0_ubuntu24.04_amd64.deb'; }
  ask() { return 0; }
  download() { printf 'new hermes package' > "$2"; }
  backup_config_for_update() { touch "$workdir/update-backup"; }
  install_local_package() { printf '%s\n' "$1" > "$workdir/update-installed"; }
  remember_asset() { printf '%s' "$2" > "$workdir/update-digest"; }
  update_hermes >/dev/null
  [ "$updated_hermes" = 1 ]
)
[ -f "$workdir/update-installed" ]
[ -s "$workdir/update-digest" ]
[ -f "$workdir/update-backup" ]

# The KMS updater must never add a missing optional driver or Gamescope.
(
  kms_installed_version() { return 1; }
  installed_package_version() { return 1; }
  gamescope_binary() { return 1; }
  release_assets() { touch "$workdir/unexpected-optional-download"; }
  update_kms >/dev/null
  update_gamescope >/dev/null
)
[ ! -e "$workdir/unexpected-optional-download" ]

# When KMS + its broker are already installed, both upgrade atomically as
# one native package manager transaction to avoid broken exact dependencies.
(
  distro=ubuntu
  distro_version=24.04
  DRY_RUN=1
  kms_installed_version() { echo 0.4.0; }
  installed_package_version() { echo 0.4.0; }
  pkg_install() { :; }
  ask() { return 0; }
  release_assets() {
    printf '%s\n' \
      'https://example.invalid/hermes-kms-dkms_0.4.1-1_all.deb' \
      'https://example.invalid/hermes-kms-seatd_0.4.1-1_all.deb'
  }
  download_checked_kms() { :; }
  install_local_package() { printf '%s\n' "$#" > "$workdir/kms-atomic-argc"; }
  update_kms >/dev/null
)
[ "$(cat "$workdir/kms-atomic-argc")" = 2 ]

# A fixed release must not be used to replace a newer pinned driver.
(
  distro=ubuntu
  kms_installed_version() { echo 0.5.0; }
  pkg_install() { touch "$workdir/unexpected-kms-downgrade"; }
  update_kms >/dev/null
)
[ ! -e "$workdir/unexpected-kms-downgrade" ]

# Gamescope rolling releases can reuse a version string. Compare artifact
# digests and skip installing when the package is unchanged.
(
  distro=ubuntu
  distro_version=24.04
  installed_package_version() { echo 0.7.0; }
  gamescope_binary() { echo /opt/hermes-gamescope/bin/gamescope; }
  release_assets() { echo 'https://example.invalid/gamescope-hermes_0.7.0_ubuntu24.04_amd64.deb'; }
  ask() { return 0; }
  download() { printf 'rolling package' > "$2"; }
  cached_asset_matches() { return 0; }
  install_local_update_package() { touch "$workdir/unexpected-gamescope-reinstall"; }
  update_gamescope >/dev/null
)
[ ! -e "$workdir/unexpected-gamescope-reinstall" ]

# Updating a stopped service must not enable or start it.
(
  updated_hermes=1
  service_was_active=0
  run() { touch "$workdir/unexpected-service-change"; }
  finish_update_service >/dev/null
)
[ ! -e "$workdir/unexpected-service-change" ]
(
  updated_hermes=1
  service_was_active=1
  ASSUME_YES=1
  run() { touch "$workdir/unexpected-service-restart"; }
  finish_update_service >/dev/null
)
[ ! -e "$workdir/unexpected-service-restart" ]

echo "Installer offline helper tests passed."
