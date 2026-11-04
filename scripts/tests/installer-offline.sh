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

echo "Installer offline helper tests passed."
