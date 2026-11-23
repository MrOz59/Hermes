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
  verify_gamescope_binary() { return 0; }
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


# Gamescope filename parsing includes the full distro-specific version.
(
  distro=arch
  [ "$(gamescope_asset_version 'https://example.invalid/gamescope-hermes-0.7.0-3-x86_64.pkg.tar.zst')" = 0.7.0-3 ]
  distro=ubuntu
  distro_version=24.04
  [ "$(gamescope_asset_version 'https://example.invalid/gamescope-hermes_0.7.0_ubuntu24.04_amd64.deb')" = 0.7.0 ]
  distro=fedora
  distro_version=44
  [ "$(gamescope_asset_version 'https://example.invalid/gamescope-hermes-0.7.0-3.fc44.x86_64.rpm')" = 0.7.0-3.fc44 ]
)

# KMS repair is opt-in and never mutates DKMS under --yes.
(
  distro=ubuntu
  ASSUME_YES=1
  kms_installed_version() { echo 0.4.1; }
  has_kms_installed() { return 1; }
  pkg_install() { touch "$workdir/unexpected-kms-repair"; }
  update_kms >/dev/null
)
[ ! -e "$workdir/unexpected-kms-repair" ]

# No automatic Gamescope downgrade even if the URL is an explicit rolling
# release selected by the updater.
(
  distro=ubuntu
  distro_version=24.04
  ASSUME_YES=1
  installed_package_version() { echo 0.9.0; }
  gamescope_binary() { echo /usr/lib/hermes-gamescope/bin/gamescope; }
  release_assets() { echo 'https://example.invalid/gamescope-hermes_0.7.0_ubuntu24.04_amd64.deb'; }
  download() { touch "$workdir/unexpected-gamescope-downgrade"; }
  update_gamescope >/dev/null
)
[ ! -e "$workdir/unexpected-gamescope-downgrade" ]

# A newer Gamescope package must use an upgrade transaction rather than
# a same-version reinstall operation.
(
  distro=ubuntu
  distro_version=24.04
  updated_gamescope=0
  installed_package_version() { echo 0.6.0; }
  gamescope_binary() { echo /usr/lib/hermes-gamescope/bin/gamescope; }
  release_assets() { echo 'https://example.invalid/gamescope-hermes_0.7.0_ubuntu24.04_amd64.deb'; }
  download() { printf 'new Gamescope package' > "$2"; }
  ask() { return 0; }
  cached_asset_matches() { return 1; }
  verify_gamescope_binary() { return 0; }
  install_local_update_package() { touch "$workdir/unexpected-gamescope-reinstall-newer"; }
  install_local_package() { touch "$workdir/gamescope-upgrade"; }
  remember_asset() { :; }
  update_gamescope >/dev/null
  [ "$updated_gamescope" = 1 ]
)
[ -e "$workdir/gamescope-upgrade" ]
[ ! -e "$workdir/unexpected-gamescope-reinstall-newer" ]

# Release asset digests: each digest is paired with its own download URL,
# whatever the field order, and assets without a digest stay unverified.
# The uploader object carries a url of its own that must not split an asset.
release_json=$(cat <<'JSON'
{
  "url": "https://api.github.com/repos/MrOz59/Hermes/releases/1",
  "assets": [
    {
      "url": "https://api.github.com/repos/MrOz59/Hermes/releases/assets/11",
      "name": "hermes_0.7.0_ubuntu26.04_amd64.deb",
      "digest": "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      "uploader": { "login": "MrOz59", "url": "https://api.github.com/users/MrOz59" },
      "browser_download_url": "https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes_0.7.0_ubuntu26.04_amd64.deb"
    },
    {
      "url": "https://api.github.com/repos/MrOz59/Hermes/releases/assets/12",
      "browser_download_url": "https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes-streaming-0.7.0-1-x86_64.pkg.tar.zst",
      "digest": "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    },
    {
      "url": "https://api.github.com/repos/MrOz59/Hermes/releases/assets/13",
      "digest": null,
      "browser_download_url": "https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes-0.7.0-1.fc44.x86_64.rpm"
    }
  ]
}
JSON
)
(
  rm -f "$workdir/asset-digests"
  record_asset_digests "$release_json"
  [ "$(asset_digest 'https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes_0.7.0_ubuntu26.04_amd64.deb')" = "$(printf 'a%.0s' {1..64})" ]
  [ "$(asset_digest 'https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes-streaming-0.7.0-1-x86_64.pkg.tar.zst')" = "$(printf 'b%.0s' {1..64})" ]
  [ -z "$(asset_digest 'https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes-0.7.0-1.fc44.x86_64.rpm')" ]
  # The same pairing holds for a response on a single line.
  rm -f "$workdir/asset-digests"
  record_asset_digests "$(printf '%s' "$release_json" | tr -d '\n')"
  [ "$(asset_digest 'https://github.com/MrOz59/Hermes/releases/download/v0.7.0/hermes-streaming-0.7.0-1-x86_64.pkg.tar.zst')" = "$(printf 'b%.0s' {1..64})" ]
)

# A download that does not match GitHub's digest is refused; a matching one
# and one with no published digest are accepted.
printf 'hermes package\n' > "$workdir/remote/hermes_0.7.0_ubuntu26.04_amd64.deb"
good_digest=$(sha256sum "$workdir/remote/hermes_0.7.0_ubuntu26.04_amd64.deb")
good_digest=${good_digest%% *}
(
  rm -f "$workdir/asset-digests"
  printf 'file://%s %s\n' "$workdir/remote/hermes_0.7.0_ubuntu26.04_amd64.deb" "$good_digest" > "$workdir/asset-digests"
  download_verified "file://$workdir/remote/hermes_0.7.0_ubuntu26.04_amd64.deb" "$workdir/downloaded/hermes.deb" >/dev/null
  download_verified "file://$workdir/remote/$filename" "$workdir/downloaded/unlisted.deb" >/dev/null 2>&1
)
if (
  printf 'file://%s %s\n' "$workdir/remote/hermes_0.7.0_ubuntu26.04_amd64.deb" "$(printf 'c%.0s' {1..64})" > "$workdir/asset-digests"
  download_verified "file://$workdir/remote/hermes_0.7.0_ubuntu26.04_amd64.deb" "$workdir/downloaded/hermes.deb" >/dev/null 2>&1
); then
  echo "FAIL: a download that does not match GitHub's digest was accepted" >&2
  exit 1
fi
rm -f "$workdir/asset-digests"

# A GitHub token is sent as a header on stdin, never as a curl argument.
(
  GITHUB_TOKEN=secret-token
  curl() {
    printf '%s\n' "$*" > "$workdir/curl-args"
    cat > "$workdir/curl-stdin"
  }
  github_api https://api.github.com/repos/MrOz59/Hermes/releases/latest
)
grep -q -- '-H @-' "$workdir/curl-args"
if grep -q secret-token "$workdir/curl-args"; then
  echo "FAIL: the GitHub token was passed on the command line" >&2
  exit 1
fi
grep -qx 'Authorization: Bearer secret-token' "$workdir/curl-stdin"

# Debug packages published beside the real ones are never selected.
(
  distro=arch
  assets='https://example.invalid/hermes-streaming-debug-0.7.0-1-x86_64.pkg.tar.zst
https://example.invalid/hermes-streaming-0.7.0-1-x86_64.pkg.tar.zst'
  [ "$(release_asset_url "$assets" "$(hermes_package_pattern)")" = 'https://example.invalid/hermes-streaming-0.7.0-1-x86_64.pkg.tar.zst' ]
  if hermes_asset_version 'https://example.invalid/hermes-streaming-debug-0.7.0-1-x86_64.pkg.tar.zst' >/dev/null; then
    echo "FAIL: a debug package was parsed as Hermes" >&2
    exit 1
  fi
  assets='https://example.invalid/gamescope-hermes-debug-3.17.0.r1.gabc1234-1-x86_64.pkg.tar.zst
https://example.invalid/gamescope-hermes-3.17.0.r1.gabc1234-1-x86_64.pkg.tar.zst'
  [ "$(release_asset_url "$assets" "$(gamescope_package_pattern)")" = 'https://example.invalid/gamescope-hermes-3.17.0.r1.gabc1234-1-x86_64.pkg.tar.zst' ]
  distro=fedora
  distro_version=44
  assets='https://example.invalid/hermes-debuginfo-0.7.0-1.fc44.x86_64.rpm
https://example.invalid/hermes-0.7.0-1.fc44.x86_64.rpm'
  [ "$(release_asset_url "$assets" "$(hermes_package_pattern)")" = 'https://example.invalid/hermes-0.7.0-1.fc44.x86_64.rpm' ]
)

# Fedora's noarch Hermes-KMS packages: the exact release when published,
# otherwise another Fedora build of the same version, never the other kind.
kms_assets='https://example.invalid/hermes-kms-seatd-0.4.1-1.fc44.noarch.rpm
https://example.invalid/hermes-kms-dkms-0.4.1-1.fc44.noarch.rpm
https://example.invalid/hermes-kms-dkms_0.4.1-1_all.deb
https://example.invalid/hermes-kms-seatd_0.4.1-1_all.deb
https://example.invalid/hermes-kms-dkms-git-0.4.1.r284.gab12cd3-1-any.pkg.tar.zst
https://example.invalid/hermes-kms-seatd-git-0.4.1.r284.gab12cd3-1-any.pkg.tar.zst
https://example.invalid/SHA256SUMS'
(
  distro=fedora
  distro_version=44
  [ "$(kms_package_url "$kms_assets" core)" = 'https://example.invalid/hermes-kms-dkms-0.4.1-1.fc44.noarch.rpm' ]
  [ "$(kms_package_url "$kms_assets" broker)" = 'https://example.invalid/hermes-kms-seatd-0.4.1-1.fc44.noarch.rpm' ]
  distro_version=45
  [ "$(kms_package_url "$kms_assets" core 2>/dev/null)" = 'https://example.invalid/hermes-kms-dkms-0.4.1-1.fc44.noarch.rpm' ]
  [ "$(kms_package_url "$kms_assets" broker 2>/dev/null)" = 'https://example.invalid/hermes-kms-seatd-0.4.1-1.fc44.noarch.rpm' ]
  distro=ubuntu
  [ "$(kms_package_url "$kms_assets" core)" = 'https://example.invalid/hermes-kms-dkms_0.4.1-1_all.deb' ]
  [ "$(kms_package_url "$kms_assets" broker)" = 'https://example.invalid/hermes-kms-seatd_0.4.1-1_all.deb' ]
  distro=arch
  [ "$(kms_package_url "$kms_assets" core)" = 'https://example.invalid/hermes-kms-dkms-git-0.4.1.r284.gab12cd3-1-any.pkg.tar.zst' ]
  [ "$(kms_package_url "$kms_assets" broker)" = 'https://example.invalid/hermes-kms-seatd-git-0.4.1.r284.gab12cd3-1-any.pkg.tar.zst' ]
)

# Config reads follow Hermes: the last assignment wins, booleans as it parses.
(
  XDG_CONFIG_HOME="$workdir/config-read"
  conf=$(config_file)
  mkdir -p "$(dirname "$conf")"
  printf 'isolation_method = gamescope\n  isolation_method =  kwin_virtual  \nhermes_kms_isolated_sessions = Enabled\n' > "$conf"
  [ "$(config_value isolation_method)" = kwin_virtual ]
  config_flag_enabled hermes_kms_isolated_sessions
  printf 'hermes_kms_isolated_sessions = false\n' > "$conf"
  if config_flag_enabled hermes_kms_isolated_sessions; then
    echo "FAIL: a disabled flag was read as enabled" >&2
    exit 1
  fi
  [ -z "$(config_value isolation_method)" ]
)

# Without a working Gamescope, a build that reads isolation_method gets
# kwin_virtual; with one, the default is left alone; 0.7.0 gets neither.
isolation_case() {
  # $1 = supports isolation_method, $2 = Gamescope ready, $3 = existing value
  (
    supports=$1 ready=$2
    XDG_CONFIG_HOME="$workdir/config-isolation-$1-$2-$3"
    config_backed_up=0
    conf=$(config_file)
    mkdir -p "$(dirname "$conf")"
    : > "$conf"
    [ "$3" = none ] || printf 'isolation_method = %s\n' "$3" > "$conf"
    hermes_supports_isolation_method() { [ "$supports" = yes ]; }
    gamescope_isolation_ready() { [ "$ready" = yes ]; }
    did_isolated=0
    [ "$ready" = yes ] && did_isolated=1
    ASSUME_YES=1
    configure_isolation_method >/dev/null
    config_value isolation_method
  )
}
[ "$(isolation_case yes no none)" = kwin_virtual ]
[ "$(isolation_case yes no gamescope)" = kwin_virtual ]
[ "$(isolation_case yes yes none)" = "" ]
[ "$(isolation_case yes no kwin_virtual)" = kwin_virtual ]
[ "$(isolation_case yes yes kwin_virtual)" = gamescope ]
[ "$(isolation_case no no none)" = "" ]

# After an update, a broken gamescope method is reported but --yes does not
# change the configuration.
(
  XDG_CONFIG_HOME="$workdir/config-update-isolation"
  conf=$(config_file)
  mkdir -p "$(dirname "$conf")"
  printf 'hermes_kms_isolated_sessions = enabled\n' > "$conf"
  hermes_supports_isolation_method() { return 0; }
  gamescope_isolation_ready() { return 1; }
  ASSUME_YES=1
  notes=()
  update_isolation_method >/dev/null
  [ -z "$(config_value isolation_method)" ]
  [ "${#notes[@]}" -eq 1 ]
)
(
  XDG_CONFIG_HOME="$workdir/config-update-isolation-ok"
  conf=$(config_file)
  mkdir -p "$(dirname "$conf")"
  printf 'hermes_kms_isolated_sessions = enabled\nisolation_method = kwin_virtual\n' > "$conf"
  hermes_supports_isolation_method() { return 0; }
  gamescope_isolation_ready() { return 1; }
  if isolation_method_broken; then
    echo "FAIL: kwin_virtual was reported as a broken isolation method" >&2
    exit 1
  fi
)

# Broker allow files keep their entries, gain the user once, stay root-owned.
(
  USER=streamer
  mkdir -p "$workdir/etc"
  existing="$workdir/etc/card-broker.allow"
  printf '# comment\nother\n' > "$existing"
  as_root() { printf '%s\n' "$*" > "$workdir/broker-install"; }
  grant_broker_user "$existing" >/dev/null
  grep -q -- '-m 0644 -o root -g root' "$workdir/broker-install"
  grep -qx other "$workdir/card-broker.allow"
  [ "$(tail -n1 "$workdir/card-broker.allow")" = streamer ]
  printf 'other\n  streamer  # Hermes\n' > "$existing"
  rm -f "$workdir/broker-install"
  grant_broker_user "$existing" >/dev/null
  [ ! -e "$workdir/broker-install" ]
)

# Both brokers stay off under --yes.
(
  ASSUME_YES=1
  DRY_RUN=0
  grant_broker_user() { touch "$workdir/unexpected-broker"; }
  as_root() { touch "$workdir/unexpected-broker"; }
  setup_optional_brokers >/dev/null
)
[ ! -e "$workdir/unexpected-broker" ]

# GitHub writes the "+" of a nightly version as %2B in download URLs; the
# installed package reports it with "+", and both must compare as the same.
(
  distro=fedora
  distro_version=45
  candidate=$(hermes_asset_version 'https://example.invalid/hermes-0.7.0%2B38c8825-1.fc45.x86_64.rpm')
  [ "$candidate" = '0.7.0+38c8825-1.fc45' ]
  [ "$(candidate_relation '0.7.0+38c8825-1.fc45' "$candidate")" = same ]
  distro=ubuntu
  distro_version=24.04
  candidate=$(hermes_asset_version 'https://example.invalid/hermes_0.7.0%2B38c8825_ubuntu24.04_amd64.deb')
  [ "$candidate" = '0.7.0+38c8825' ]
  [ "$(url_basename 'https://example.invalid/hermes_0.7.0%2B38c8825_ubuntu24.04_amd64.deb')" = 'hermes_0.7.0+38c8825_ubuntu24.04_amd64.deb' ]
)

# A release lookup that fails (offline, rate limited) aborts; it is not a
# missing package and never offers the nightly channel.
if (
  CHANNEL=release
  distro=ubuntu
  distro_version=24.04
  release_assets() {
    [ "$2" != nightly ] || touch "$workdir/nightly-after-api-failure"
    return 1
  }
  ask() { touch "$workdir/asked-after-api-failure"; return 0; }
  install_hermes >/dev/null 2>&1
); then
  echo "FAIL: a failed release lookup did not abort" >&2
  exit 1
fi
[ ! -e "$workdir/nightly-after-api-failure" ]
[ ! -e "$workdir/asked-after-api-failure" ]

echo "Installer offline helper tests passed."
