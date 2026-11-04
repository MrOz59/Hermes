#!/usr/bin/env bash
# Hermes installer.
#
# Installs Hermes and, one question at a time, the pieces around it: the
# Hermes-KMS virtual display driver, what experimental isolated sessions need
# (including Hermes' own Gamescope build), and streaming the login screen
# before anyone has logged in. Nothing optional is installed without asking.
#
# Run it as the user Hermes will stream for, not as root; it asks sudo for the
# steps that need it:
#
#   curl -fsSL https://raw.githubusercontent.com/MrOz59/Hermes/main/scripts/install.sh | bash
#
# or, from a checkout, `scripts/install.sh --help` for the options.
set -euo pipefail

HERMES_REPO="MrOz59/Hermes"
KMS_REPO="MrOz59/Hermes-KMS"
KMS_RELEASE="v0.4.1"  # Stable release matching Hermes 0.7.0 / UAPI 14
GAMESCOPE_REPO="MrOz59/gamescope"
GAMESCOPE_BRANCH="hermes"
GAMESCOPE_RELEASE="hermes-nightly"
GAMESCOPE_SOURCE_PREFIX="/opt/hermes-gamescope"

ASSUME_YES=0
DRY_RUN=0
CHANNEL="release"
MODE="install"

# What was decided, for the summary and for steps that depend on each other.
did_kms=0
kms_ready=0
broker_from_source=0
did_detached=0
did_broker=0
did_isolated=0
did_gamescope=0
did_prelogin=0
need_reboot=0
config_changed=0
config_backed_up=0
service_was_active=0
updated_hermes=0
updated_kms=0
updated_gamescope=0
updated_evdi=0
kms_source_dir=""
notes=()

# ---------------------------------------------------------------------------
# Output and questions

if [ -t 1 ]; then
  bold=$'\e[1m' dim=$'\e[2m' red=$'\e[31m' green=$'\e[32m' yellow=$'\e[33m' reset=$'\e[0m'
else
  bold='' dim='' red='' green='' yellow='' reset=''
fi

step() { printf '\n%s==> %s%s\n' "$bold" "$*" "$reset"; }
info() { printf '    %s\n' "$*"; }
ok() { printf '    %s%s%s\n' "$green" "$*" "$reset"; }
warn() { printf '    %s%s%s\n' "$yellow" "$*" "$reset" >&2; }
die() {
  printf '%serror:%s %s\n' "$red" "$reset" "$*" >&2
  exit 1
}

# Explanations are printed as a block, so a question is never asked about
# something the user has not been told about.
explain() {
  local line
  while IFS= read -r line; do
    printf '    %s%s%s\n' "$dim" "$line" "$reset"
  done
}

# ask "Question" y|n -> 0 for yes. Reads the terminal even when the script
# itself arrives on stdin (curl | bash); with --yes, or with no terminal at
# all, the default is taken and said.
ask() {
  local question=$1 default=$2 hint answer
  [ "$default" = y ] && hint='[Y/n]' || hint='[y/N]'
  if [ "$ASSUME_YES" = 1 ] || ! { : </dev/tty; } 2>/dev/null; then
    printf '    %s %s %s\n' "$question" "$hint" "$default"
    [ "$default" = y ]
    return
  fi
  while true; do
    printf '    %s%s%s %s ' "$bold" "$question" "$reset" "$hint"
    IFS= read -r answer </dev/tty || answer=''
    answer=${answer:-$default}
    case "${answer,,}" in
      y | yes) return 0 ;;
      n | no) return 1 ;;
    esac
  done
}

# Commands are shown before they run, and only shown with --dry-run.
run() {
  printf '    %s$ %s%s\n' "$dim" "$*" "$reset"
  [ "$DRY_RUN" = 1 ] || "$@"
}

as_root() {
  if [ "$(id -u)" -eq 0 ]; then
    run "$@"
  else
    run sudo "$@"
  fi
}

# ---------------------------------------------------------------------------
# The machine

usage() {
  cat <<EOF
Usage: install.sh [options]

Installs or updates Hermes and its existing ecosystem components.

Options:
  --update    update installed Hermes and existing components only
  --nightly   install the rolling nightly build of main instead of the
              latest release (KMS remains pinned to v0.4.1 for Hermes 0.7.0)
  --yes       take the default answer to every question (optional pieces
              that default to no stay off)
  --dry-run   show what would run without changing anything
  -h, --help  show this help
EOF
}

distro=""
distro_version=""

detect_distro() {
  [ -r /etc/os-release ] || die "cannot read /etc/os-release; this installer supports Arch, CachyOS, Ubuntu and Fedora."
  # shellcheck disable=SC1091
  . /etc/os-release
  local ids=" ${ID:-} ${ID_LIKE:-} "
  distro_version=${VERSION_ID:-}
  case "$ids" in
    *" arch "* | *" cachyos "*) distro=arch ;;
    *" fedora "*) distro=fedora ;;
    *" ubuntu "*) distro=ubuntu ;;
    *) die "${PRETTY_NAME:-this distribution} is not supported here. Hermes packages exist for Arch/CachyOS, Ubuntu 24.04/26.04 and Fedora 43/44; see https://github.com/${HERMES_REPO}#installing." ;;
  esac
  # Derivatives report their own VERSION_ID; the packages follow the base.
  if [ "$distro" = ubuntu ] && [ "${ID:-}" != ubuntu ] && [ -n "${UBUNTU_CODENAME:-}" ]; then
    case "$UBUNTU_CODENAME" in
      noble) distro_version=24.04 ;;
      resolute) distro_version=26.04 ;;
    esac
  fi
}

pkg_install() {
  case "$distro" in
    arch) as_root pacman -S --needed --noconfirm "$@" ;;
    ubuntu) as_root apt-get install -y "$@" ;;
    fedora) as_root dnf install -y "$@" ;;
  esac
}

install_local_package() {
  case "$distro" in
    arch) as_root pacman -U --noconfirm "$@" ;;
    ubuntu) as_root apt-get install -y "$@" ;;
    fedora) as_root dnf install -y "$@" ;;
  esac
}

workdir=""
cleanup() {
  [ -z "$workdir" ] || rm -rf -- "$workdir"
}
trap cleanup EXIT

# The download URLs of a release's assets, one per line. The GitHub API is
# read without a token, so its rate limit (60 requests an hour) applies.
release_assets() {
  local repo=$1 tag=$2 url json
  if [ "$tag" = latest ]; then
    url="https://api.github.com/repos/${repo}/releases/latest"
  else
    url="https://api.github.com/repos/${repo}/releases/tags/${tag}"
  fi
  json=$(curl -fsSL -H 'Accept: application/vnd.github+json' "$url") || return 1
  printf '%s\n' "$json" | grep -o '"browser_download_url": *"[^"]*"' | sed 's/.*"\(https[^"]*\)"$/\1/'
}

download() {
  local url=$1 dest=$2
  run curl -fL --progress-bar -o "$dest" "$url"
}

kernel_headers_package() {
  case "$distro" in
    arch)
      # Arch kernels record the package they came from.
      local base
      base=$(cat "/usr/lib/modules/$(uname -r)/pkgbase" 2>/dev/null || echo linux)
      echo "${base}-headers"
      ;;
    ubuntu) echo "linux-headers-$(uname -r)" ;;
    fedora) echo "kernel-devel-$(uname -r)" ;;
  esac
}

# Gamescope's wlroots needs newer libwayland, libdrm and libxkbcommon than
# Ubuntu 24.04 ships, so neither a package nor a source build of Hermes'
# Gamescope exists there.
gamescope_unavailable() {
  [ "$distro" = ubuntu ] && [ "$distro_version" = 24.04 ]
}

config_file() {
  echo "${XDG_CONFIG_HOME:-$HOME/.config}/hermes/hermes.conf"
}

# Set key = value in Hermes' config, replacing an existing line for the key.
# Preserve user configuration, avoid repeated writes, and back up once.
set_config() {
  local key=$1 value=$2 file backup
  file=$(config_file)
  printf '    %s%s: %s = %s%s\n' "$dim" "$file" "$key" "$value" "$reset"
  [ "$DRY_RUN" = 1 ] && return 0
  if [ -L "$file" ]; then
    warn "Refusing to edit symlinked configuration: $file"
    return 1
  fi
  mkdir -p "$(dirname "$file")"
  if [ -f "$file" ] && grep -Eq "^[[:space:]]*${key}[[:space:]]*=[[:space:]]*${value}[[:space:]]*$" "$file"; then
    return 0
  fi
  if [ -f "$file" ] && [ "$config_backed_up" = 0 ]; then
    backup=$(mktemp "${file}.bak.XXXXXXXX")
    cp -p -- "$file" "$backup"
    ok "Backed up Hermes configuration to $backup"
    config_backed_up=1
  fi
  touch "$file"
  if grep -Eq "^[[:space:]]*${key}[[:space:]]*=" "$file"; then
    sed -i -E "s|^[[:space:]]*${key}[[:space:]]*=.*|${key} = ${value}|" "$file"
  else
    printf '%s = %s\n' "$key" "$value" >>"$file"
  fi
  config_changed=1
}


release_asset_url() {
  local assets=$1 pattern=$2
  printf '%s\n' "$assets" | grep -E "$pattern" | head -n1 || true
}

download_checked_kms() {
  local assets=$1 url=$2 dest=$3 checksum_url checksum_file expected actual
  checksum_url=$(release_asset_url "$assets" '/SHA256SUMS$')
  [ -n "$checksum_url" ] || die "No SHA256SUMS in Hermes-KMS ${KMS_RELEASE}."
  download "$url" "$dest"
  checksum_file="${workdir}/kms-SHA256SUMS"
  download "$checksum_url" "$checksum_file"
  [ "$DRY_RUN" = 1 ] && return 0
  expected=$(awk -v filename="${dest##*/}" '$2 == filename || $2 == "*" filename {print $1; exit}' "$checksum_file")
  [ "${#expected}" -eq 64 ] || die "Missing SHA-256 checksum for ${dest##*/}."
  actual=$(sha256sum "$dest")
  [ "${actual%% *}" = "$expected" ] || die "Checksum mismatch for ${dest##*/}; aborting."
  ok "Verified ${dest##*/} against the release checksums."
}

# DKMS checks must match the complete version and running kernel.
has_kms_installed() {
  [ "$DRY_RUN" = 0 ] || return 1
  command -v dkms >/dev/null 2>&1 || return 1
  dkms status -m hermes-kms 2>/dev/null | awk -F '[/,:]' \
    -v expected_version="${KMS_RELEASE#v}" -v expected_kernel="$(uname -r)" -v family="$distro" '
    $1 == "hermes-kms" {
      # Arch -git packages use 0.4.1.rN.gSHA; other packages use 0.4.1.
      expected_prefix = expected_version
      gsub(/[.]/, "[.]", expected_prefix)
      version_ok = $2 == expected_version ||
                   (family == "arch" && $2 ~ ("^" expected_prefix "[.]r[0-9]+[.]g[0-9a-fA-F]+$"))
      kernel = $3
      state = $NF
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", kernel)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", state)
      if (version_ok && kernel == expected_kernel && state ~ /^installed([[:space:]]|$)/) found = 1
    }
    END { exit !found }'
}

verify_kms_installed() {
  [ "$DRY_RUN" = 1 ] && return 0
  if ! has_kms_installed; then
    warn "DKMS does not report hermes-kms ${KMS_RELEASE#v} installed for $(uname -r)."
    warn "Inspect: dkms status -m hermes-kms"
    return 1
  fi
  ok "Hermes-KMS DKMS ${KMS_RELEASE#v} installed for $(uname -r)."
}

kms_runtime_diagnostics() {
  [ "$DRY_RUN" = 1 ] && return 0
  if [ -d /sys/module/hermes_kms ]; then
    ok "Hermes-KMS module is loaded."
  elif [ "$need_reboot" = 1 ]; then
    warn "Hermes-KMS is installed but not loaded. Reboot before testing."
  else
    warn "Hermes-KMS is installed but not loaded; inspect modprobe hermes_kms and journalctl -k."
  fi
  if command -v modinfo >/dev/null 2>&1; then
    local mod_version
    mod_version=$(modinfo -F version hermes_kms 2>/dev/null || true)
    if [ -n "$mod_version" ] && [ "$mod_version" != "${KMS_RELEASE#v}" ]; then
      warn "Module on disk reports $mod_version (expected ${KMS_RELEASE#v})."
    fi
  fi
  if command -v mokutil >/dev/null 2>&1 &&
     mokutil --sb-state 2>/dev/null | grep -qi 'secureboot enabled' &&
     [ ! -d /sys/module/hermes_kms ]; then
    warn "Secure Boot is enabled. DKMS may require signing and MOK enrollment."
    notes+=("Check distribution MOK enrollment; this installer will not disable Secure Boot.")
  fi
}


# The package manager is authoritative; the digest cache avoids repeated
# installation of rolling assets which may keep the same version string.
installer_state_dir() {
  printf '%s\n' "${XDG_STATE_HOME:-$HOME/.local/state}/hermes/installer"
}

installed_package_version() {
  local pkg=$1
  case "$distro" in
    arch) command -v pacman >/dev/null 2>&1 || return 1
      pacman -Q "$pkg" 2>/dev/null | awk 'NR == 1 { print $2 }' ;;
    ubuntu) command -v dpkg-query >/dev/null 2>&1 || return 1
      dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null ;;
    fedora) command -v rpm >/dev/null 2>&1 || return 1
      rpm -q --qf '%{VERSION}-%{RELEASE}\n' "$pkg" 2>/dev/null ;;
  esac
}

hermes_package_name() {
  case "$distro" in arch) echo hermes-streaming ;; *) echo hermes ;; esac
}
kms_core_package_name() {
  case "$distro" in arch) echo hermes-kms-dkms-git ;; *) echo hermes-kms-dkms ;; esac
}
kms_broker_package_name() {
  case "$distro" in arch) echo hermes-kms-seatd-git ;; *) echo hermes-kms-seatd ;; esac
}

gamescope_package_name() { echo gamescope-hermes; }

hermes_asset_version() {
  local name=${1##*/}
  case "$distro" in
    arch) [[ "$name" == hermes-streaming-*-x86_64.pkg.tar.zst ]] || return 1
      name=${name#hermes-streaming-}
      echo "${name%-x86_64.pkg.tar.zst}" ;;
    ubuntu) [[ "$name" == hermes_*_ubuntu${distro_version}_amd64.deb ]] || return 1
      name=${name#hermes_}
      echo "${name%_ubuntu${distro_version}_amd64.deb}" ;;
    fedora) [[ "$name" == hermes-*.fc${distro_version}.x86_64.rpm ]] || return 1
      name=${name#hermes-}
      echo "${name%.x86_64.rpm}" ;;
  esac
}

# Prints newer, older or same for candidate relative to installed version.
candidate_relation() {
  local installed=$1 candidate=$2 result first
  if [ "$installed" = "$candidate" ]; then echo same
  elif [ "$distro" = ubuntu ] && command -v dpkg >/dev/null 2>&1; then
    if dpkg --compare-versions "$candidate" gt "$installed"; then echo newer; else echo older; fi
  elif [ "$distro" = arch ] && command -v vercmp >/dev/null 2>&1; then
    result=$(vercmp "$candidate" "$installed")
    if [ "$result" -gt 0 ]; then echo newer
    elif [ "$result" -lt 0 ]; then echo older
    else echo same; fi
  else
    first=$(printf '%s\n%s\n' "$installed" "$candidate" | LC_ALL=C sort -V | head -n1)
    if [ "$first" = "$candidate" ]; then echo older; else echo newer; fi
  fi
}

cached_asset_matches() {
  local component=$1 digest=$2 file
  [ "$DRY_RUN" = 0 ] || return 1
  file="$(installer_state_dir)/${component}.sha256"
  [ -f "$file" ] && [ "$(cat "$file")" = "$digest" ]
}

remember_asset() {
  [ "$DRY_RUN" = 0 ] || return 0
  local component=$1 digest=$2 path
  path="$(installer_state_dir)/${component}.sha256"
  mkdir -p "$(dirname "$path")"
  [ ! -L "$path" ] || { warn "Refusing symlinked installer cache: $path"; return 1; }
  printf '%s\n' "$digest" >"$path"
}

backup_config_for_update() {
  [ "$DRY_RUN" = 0 ] || return 0
  [ "$config_backed_up" = 0 ] || return 0
  local file backup
  file=$(config_file)
  [ -f "$file" ] || return 0
  [ ! -L "$file" ] || { warn "Refusing symlinked Hermes configuration: $file"; return 1; }
  backup=$(mktemp "${file}.bak.XXXXXXXX")
  cp -p -- "$file" "$backup"
  config_backed_up=1
  ok "Preserved Hermes configuration at $backup"
}

kms_installed_version() {
  local version
  version=$(installed_package_version "$(kms_core_package_name)" || true)
  if [ -n "$version" ]; then echo "$version"; return 0; fi
  command -v dkms >/dev/null 2>&1 || return 1
  dkms status -m hermes-kms 2>/dev/null |
    awk -F '[/,:]' '$1 == "hermes-kms" && $2 != "" { print $2; exit }'
}

kms_base_version() {
  local version=$1
  if [[ "$version" =~ ^([0-9]+[.][0-9]+[.][0-9]+) ]]; then
    echo "${BASH_REMATCH[1]}"
  else
    echo "$version"
  fi
}

# ---------------------------------------------------------------------------
# Steps

preflight() {
  [ "$(uname -s)" = Linux ] || die "this installer is for Linux."
  [ "$(uname -m)" = x86_64 ] || die "Hermes packages are built for x86_64 only."
  if [ "$(id -u)" -eq 0 ]; then
    die "run this as the user Hermes will stream for, not as root; it uses sudo where it must."
  fi
  command -v systemctl >/dev/null || die "Hermes runs as a systemd user service; systemctl was not found."
  command -v curl >/dev/null || die "curl is required."
  command -v sudo >/dev/null || die "sudo is required."
  command -v sha256sum >/dev/null || die "sha256sum is required for checked driver packages."
  if [ -e /run/ostree-booted ] || [ -e /usr/lib/ostree-booted ]; then
    die "Immutable/ostree hosts need the Hermes-KMS image-based installation; this DKMS installer is for mutable distributions."
  fi
  detect_distro
  info "Distribution: ${distro} ${distro_version}, kernel $(uname -r), user ${USER}."
  if systemctl --user is-active --quiet hermes; then
    service_was_active=1
  fi
  if [ "$DRY_RUN" = 0 ]; then
    info "sudo will ask for your password once."
    sudo -v || die "sudo is needed to install packages."
  fi
  workdir=$(mktemp -d -t hermes-install.XXXXXX)
  if [ "$distro" = ubuntu ]; then
    as_root apt-get update
  fi
}

install_hermes() {
  step "Hermes"
  local tag=latest pattern assets url file
  [ "$CHANNEL" = nightly ] && tag=nightly
  case "$distro" in
    arch) pattern='hermes-streaming-.*-x86_64\.pkg\.tar\.zst$' ;;
    ubuntu) pattern="hermes_.*_ubuntu${distro_version}_amd64\.deb$" ;;
    fedora) pattern="hermes-.*\.fc${distro_version}\.x86_64\.rpm$" ;;
  esac
  info "Looking for the ${CHANNEL} package in ${HERMES_REPO}..."
  assets=$(release_assets "$HERMES_REPO" "$tag") || {
    [ "$CHANNEL" = release ] || die "could not read the nightly release from GitHub."
    assets=''
  }
  url=$(printf '%s\n' "$assets" | grep -E "$pattern" | head -n1 || true)
  if [ -z "$url" ]; then
    if [ "$CHANNEL" = release ]; then
      warn "No stable release package for ${distro} ${distro_version}."
      if ask "Use the rolling nightly build instead (experimental)?" n; then
        assets=$(release_assets "$HERMES_REPO" nightly) || die "could not read the nightly release from GitHub."
        url=$(printf '%s\n' "$assets" | grep -E "$pattern" | head -n1 || true)
        CHANNEL=nightly
      fi
    fi
    [ -n "$url" ] || die "no Hermes package for ${distro} ${distro_version} in the releases at https://github.com/${HERMES_REPO}/releases."
  fi
  file="${workdir}/${url##*/}"
  download "$url" "$file"
  install_local_package "$file"
  ok "Hermes installed."
}

install_kms() {
  step "Hermes-KMS virtual display driver"
  explain <<'EOF'
Hermes-KMS provides virtual displays with zero-copy capture. It is needed for
independent sessions and streaming the login screen; EVDI is an alternative for
basic streaming. We use the published Hermes-KMS 0.4.1 release for Hermes 0.7.0,
rather than cloning a changing main branch. This kernel module uses DKMS.
EOF
  if ! ask "Install Hermes-KMS?" y; then
    if has_kms_installed; then
      kms_ready=1
      info "Using the installed Hermes-KMS module."
    fi
    return 0
  fi

  local headers pattern assets url file src
  headers=$(kernel_headers_package)
  case "$distro" in
    arch)
      pkg_install base-devel dkms "$headers"
      pattern='/hermes-kms-dkms-git-.*-any\.pkg\.tar\.zst$'
      ;;
    ubuntu)
      pkg_install build-essential dkms "$headers"
      pattern='/hermes-kms-dkms_.*_all\.deb$'
      ;;
    fedora)
      pkg_install make gcc dkms "$headers"
      pattern="/hermes-kms-dkms-.*\.fc${distro_version}\.noarch\.rpm$"
      ;;
  esac
  if [ "$DRY_RUN" = 0 ] && [ ! -f "/lib/modules/$(uname -r)/build/Makefile" ]; then
    warn "No matching kernel headers for $(uname -r); Hermes-KMS installation skipped."
    return 0
  fi

  assets=$(release_assets "$KMS_REPO" "$KMS_RELEASE") ||
    die "Cannot read Hermes-KMS ${KMS_RELEASE} release metadata."
  url=$(release_asset_url "$assets" "$pattern")
  if [ -n "$url" ]; then
    file="${workdir}/${url##*/}"
    download_checked_kms "$assets" "$url" "$file"
    install_local_package "$file"
  else
    warn "No matching ${KMS_RELEASE} driver package for ${distro} ${distro_version}."
    explain <<'EOF'
You can build this exact stable release tag via DKMS instead. This avoids
fetching unreviewed changes from the moving development branch.
EOF
    if ! ask "Build Hermes-KMS from its stable release tag?" n; then
      notes+=("Hermes-KMS was not installed; check the driver's release packages for your distribution.")
      return 0
    fi
    pkg_install git
    src="${workdir}/Hermes-KMS"
    run git clone --depth 1 --branch "$KMS_RELEASE" "https://github.com/${KMS_REPO}.git" "$src"
    # Install only core module/udev files; brokers stay explicitly optional.
    as_root make -C "$src" dkms-install
    kms_source_dir="$src"
    broker_from_source=1
  fi

  if ! verify_kms_installed; then
    notes+=("Hermes-KMS DKMS verification failed; do not enable independent sessions yet.")
    return 0
  fi
  did_kms=1
  kms_ready=1
  need_reboot=1

  explain <<'EOF'
EVDI remains the default Hermes virtual-display backend. Select Hermes-KMS
to enable its zero-copy path and the independent-session capabilities.
EOF
  if ask "Make Hermes-KMS Hermes' virtual display backend?" y; then
    set_config virtual_display_backend hermes_kms
  else
    notes+=("Hermes-KMS installed but not selected: set virtual_display_backend = hermes_kms in $(config_file) to use it.")
  fi
  ok "Hermes-KMS installation completed. Reboot before testing."
}

# The default Plasma Detached Desktop does not require this private-seat helper.
install_kms_broker() {
  step "Private DRM card/seat broker for Gamescope sessions"
  local assets url pattern file rc
  pkg_install acl seatd
  if [ "$broker_from_source" = 1 ]; then
    [ -n "$kms_source_dir" ] || { warn "Missing pinned Hermes-KMS source for broker setup."; return 1; }
    as_root make -C "$kms_source_dir" install-broker-configs
    as_root systemctl daemon-reload
    as_root udevadm control --reload-rules
  else
    assets=$(release_assets "$KMS_REPO" "$KMS_RELEASE") || die "Cannot read Hermes-KMS broker release."
    case "$distro" in
      arch) pattern='/hermes-kms-seatd-git-.*-any\.pkg\.tar\.zst$' ;;
      ubuntu) pattern='/hermes-kms-seatd_.*_all\.deb$' ;;
      fedora) pattern="/hermes-kms-seatd-.*\.fc${distro_version}\.noarch\.rpm$" ;;
    esac
    url=$(release_asset_url "$assets" "$pattern")
    if [ -z "$url" ]; then
      warn "No matching private-seat package for ${distro} ${distro_version}."
      return 1
    fi
    file="${workdir}/${url##*/}"
    download_checked_kms "$assets" "$url" "$file"
    install_local_package "$file"
  fi

  if [ "$DRY_RUN" = 0 ] && [ ! -x /usr/lib/hermes-kms/hermes-kms-setup ]; then
    warn "Missing /usr/lib/hermes-kms/hermes-kms-setup after package install."
    return 1
  fi
  info "Configuring the private-seat pool for UID $(id -u), not the root user."
  rc=0
  as_root /usr/lib/hermes-kms/hermes-kms-setup configure --user "$(id -u)" || rc=$?
  if [ "$rc" -ne 0 ] && [ "$rc" -ne 10 ]; then
    warn "Private seat setup failed (exit ${rc}). Gamescope isolation will not be enabled."
    return 1
  fi
  if [ "$rc" -eq 10 ]; then
    warn "The updated seat/card topology will be active after a reboot."
    need_reboot=1
  fi
  did_broker=1
  ok "Private-seat broker is configured."
  return 0
}

# Private DRM application sessions need the fork's DRM device-selection flag.
gamescope_binary() {
  local candidate
  for candidate in /usr/lib/hermes-gamescope/bin/gamescope /opt/hermes-gamescope/bin/gamescope; do
    if [ -x "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

verify_gamescope_binary() {
  [ "$DRY_RUN" = 1 ] && return 0
  local binary help_text
  binary=$(gamescope_binary) || { warn "Patched Gamescope executable not found."; return 1; }
  if ! help_text=$("$binary" --help 2>&1); then
    warn "Gamescope --help failed for $binary; check its runtime dependencies."
    return 1
  fi
  if ! grep -Eq -- '--prefer-drm|--drm-device' <<<"$help_text"; then
    warn "Gamescope at $binary lacks the required private DRM override option."
    return 1
  fi
  ok "Verified Gamescope DRM option: $binary"
}

install_gamescope() {
  explain <<EOF
Isolated application sessions run the game in Gamescope, on the session's own
Hermes-KMS card and seat. Stock Gamescope can do neither: it scans out on the
GPU it renders with - your desktop's display - and reads the host's keyboard
and mouse. Hermes' build (github.com/${GAMESCOPE_REPO}, branch ${GAMESCOPE_BRANCH})
carries the patches for both. It installs beside your distribution's
Gamescope, in its own directory, so the Gamescope you already use is left
alone; Hermes looks for its own build first.
EOF
  ask "Install Hermes' Gamescope build?" y || {
    notes+=("Isolated application sessions need a Gamescope that can scan out on another device; they will refuse to start with the stock one.")
    return 0
  }

  # The fork's CI builds a package per distribution release, like Hermes'
  # own, and keeps them on a rolling release.
  local pattern assets url file src
  case "$distro" in
    arch) pattern='gamescope-hermes-.*-x86_64\.pkg\.tar\.zst$' ;;
    ubuntu) pattern="gamescope-hermes_.*_ubuntu${distro_version}_amd64\.deb$" ;;
    fedora) pattern="gamescope-hermes-.*\.fc${distro_version}\.x86_64\.rpm$" ;;
  esac
  if assets=$(release_assets "$GAMESCOPE_REPO" "$GAMESCOPE_RELEASE" 2>/dev/null) &&
    url=$(printf '%s\n' "$assets" | grep -E "$pattern" | head -n1) &&
    [ -n "$url" ]; then
    file="${workdir}/${url##*/}"
    download "$url" "$file"
    install_local_package "$file"
    if ! verify_gamescope_binary; then
      notes+=("Gamescope was installed but its private DRM interface could not be verified.")
      return 0
    fi
    did_gamescope=1
    ok "Gamescope for Hermes installed in /usr/lib/hermes-gamescope."
    return 0
  fi

  if [ "$distro" = arch ]; then
    info "No prebuilt package was found; building it (a few minutes)."
    pkg_install base-devel git
    src="${workdir}/gamescope"
    run git clone --recursive --branch "$GAMESCOPE_BRANCH" "https://github.com/${GAMESCOPE_REPO}.git" "$src"
    if [ "$DRY_RUN" = 0 ]; then
      (cd "$src/packaging/hermes" && run makepkg -si --noconfirm --needed)
    else
      run makepkg -si --noconfirm --needed
    fi
    if ! verify_gamescope_binary; then
      notes+=("Gamescope was installed but its private DRM interface could not be verified.")
      return 0
    fi
    did_gamescope=1
    ok "Gamescope for Hermes installed in /usr/lib/hermes-gamescope."
    return 0
  fi

  if gamescope_unavailable; then
    warn "Isolated sessions through Gamescope are unavailable on Ubuntu 24.04 for now."
    return 0
  fi

  explain <<EOF
No package was found for ${distro} ${distro_version}; it can be built from
source into ${GAMESCOPE_SOURCE_PREFIX}, the other place Hermes looks. Gamescope
needs recent libraries, and the build may fail on an older release. The WSI
layer is left out, since it needs newer Vulkan headers than most releases ship.
EOF
  ask "Build it from source?" n || {
    notes+=("Isolated application sessions need Hermes' Gamescope; see https://github.com/${GAMESCOPE_REPO}/blob/${GAMESCOPE_BRANCH}/HERMES.md.")
    return 0
  }
  case "$distro" in
    ubuntu)
      pkg_install git meson ninja-build cmake pkg-config glslang-tools libvulkan-dev \
        libx11-dev libxmu-dev libxcomposite-dev libxrender-dev libxres-dev libxtst-dev \
        libxkbcommon-dev libdrm-dev libinput-dev libseat-dev libwayland-dev wayland-protocols \
        libxdamage-dev libxcursor-dev libxxf86vm-dev libxi-dev libpixman-1-dev libudev-dev \
        libdecor-0-dev libluajit-5.1-dev libsdl2-dev libavif-dev libcap-dev hwdata \
        libpipewire-0.3-dev libxcb-composite0-dev libxcb-res0-dev libxcb-icccm4-dev \
        libxcb-errors-dev libglm-dev xwayland libei-dev ||
        warn "Some build dependencies did not install; the build will say which are missing."
      ;;
    fedora)
      pkg_install git meson ninja-build cmake gcc-c++ glslang vulkan-headers vulkan-loader-devel \
        libX11-devel libXmu-devel libXcomposite-devel libXrender-devel libXres-devel libXtst-devel \
        libxkbcommon-devel libdrm-devel libinput-devel libseat-devel wayland-devel wayland-protocols-devel \
        libXdamage-devel libXcursor-devel libXxf86vm-devel libXi-devel pixman-devel systemd-devel \
        libdecor-devel luajit-devel SDL2-devel libavif-devel libcap-devel hwdata-devel \
        pipewire-devel xcb-util-wm-devel xcb-util-errors-devel glm-devel xorg-x11-server-Xwayland libei-devel ||
        warn "Some build dependencies did not install; the build will say which are missing."
      ;;
  esac
  local src="${workdir}/gamescope"
  run git clone --recursive --branch "$GAMESCOPE_BRANCH" "https://github.com/${GAMESCOPE_REPO}.git" "$src"
  run meson setup "$src/build" "$src" --prefix="$GAMESCOPE_SOURCE_PREFIX" --buildtype=release \
    -Dforce_fallback_for=libliftoff,vkroots,wlroots,libdisplay-info \
    -Denable_openvr_support=false -Denable_tests=false -Denable_gamescope_wsi_layer=false
  run ninja -C "$src/build"
  as_root meson install -C "$src/build" --skip-subprojects
  if ! verify_gamescope_binary; then
    notes+=("Built Gamescope, but its private DRM interface could not be verified.")
    return 0
  fi
  did_gamescope=1
  ok "Gamescope for Hermes installed in ${GAMESCOPE_SOURCE_PREFIX}."
}

# EVDI remains the default Hermes backend when Hermes-KMS is not selected.
# Do not silently install an AUR or third-party Fedora package.
has_evdi_installed() {
  [ -d /sys/module/evdi ] ||
    { command -v modinfo >/dev/null 2>&1 && modinfo evdi >/dev/null 2>&1; }
}

install_evdi_if_needed() {
  [ "$kms_ready" = 0 ] || return 0
  step "Alternative virtual display backend: EVDI"
  if has_evdi_installed; then
    ok "EVDI is already available; ordinary virtual-display streaming can use it."
    return 0
  fi
  explain <<'EOF'
Hermes-KMS was not selected, and no EVDI module was detected. Existing physical
desktop capture may still work, but virtual-display streaming needs a backend.
EVDI is the default, stable virtual-display backend; it does not support the
independent-session features offered by Hermes-KMS.
EOF
  if ! ask "Install EVDI for ordinary virtual displays?" n; then
    notes+=("No EVDI or Hermes-KMS module detected; install a virtual-display backend if needed.")
    return 0
  fi
  case "$distro" in
    arch)
      if command -v paru >/dev/null 2>&1; then
        run paru -S --needed --noconfirm evdi
      elif command -v yay >/dev/null 2>&1; then
        run yay -S --needed --noconfirm evdi
      else
        warn "An AUR helper (paru or yay) is required for the Arch evdi package."
        notes+=("Install EVDI from the AUR manually, or rerun to install Hermes-KMS.")
        return 0
      fi
      ;;
    ubuntu)
      if apt-cache show evdi-dkms >/dev/null 2>&1; then
        pkg_install evdi-dkms
      else
        warn "evdi-dkms is not available from the configured APT repositories."
        notes+=("See docs/getting_started.md for the available EVDI installation options.")
        return 0
      fi
      ;;
    fedora)
      warn "EVDI is not provided by the default Fedora repositories."
      notes+=("Use a trusted EVDI package for this Fedora kernel, or choose Hermes-KMS.")
      return 0
      ;;
  esac
  if [ "$DRY_RUN" = 0 ] && ! has_evdi_installed; then
    warn "EVDI was installed but a module for the running kernel was not found."
    notes+=("Check EVDI DKMS build logs and matching kernel headers.")
    return 0
  fi
  need_reboot=1
  ok "EVDI package installed. Reboot before using a virtual display."
}

install_isolated() {
  step "Independent sessions — Hermes 0.7.0 (experimental)"
  explain <<'EOF'
Hermes has TWO ways to keep remote streams separate from the host desktop:

DETACHED DESKTOP / DETACHED STEAM (KDE Plasma 6, Wayland)
  Built with help from PunkUnity's work. Your own Plasma account, files,
  settings and Steam library run on a detached virtual output; the physical
  desktop stays usable and its keyboard/mouse do not control the remote view.
  These modes do NOT need a patched Gamescope or private-seat broker.

ISOLATED APPLICATION (Gamescope DRM)
  A game runs in its own patched Gamescope on a dedicated Hermes-KMS DRM card
  and input seat, with separate PipeWire audio and optional private devices.
  This needs a private-seat broker, seatd and Hermes' Gamescope build.

Both are experimental, per-app choices in the Web UI. Normal Desktop and
shared-host streams remain available. Future releases may change these modes.
EOF
  if [ "$kms_ready" = 0 ]; then
    warn "Skipping independent sessions: a working Hermes-KMS driver is required."
    return 0
  fi
  if ! ask "Enable independent sessions (including Detached Desktop/Steam)?" n; then
    return 0
  fi
  if [ "${XDG_SESSION_TYPE:-}" != wayland ] ||
     [[ "${XDG_CURRENT_DESKTOP:-}" != *KDE* && "${KDE_FULL_SESSION:-}" != true ]]; then
    warn "Detached Desktop/Steam needs an active KDE Plasma 6 Wayland desktop."
    warn "You can enable the feature now, but it will not work on an X11/other desktop."
  fi

  set_config virtual_display_backend hermes_kms
  set_config hermes_kms_isolated_sessions enabled
  if [ "$DRY_RUN" = 1 ] || [ -x /usr/bin/hermes-detached-session ]; then
    did_detached=1
    ok "Independent-session capability enabled; shared streaming remains available."
  else
    warn "Detached Desktop/Steam helper is absent; check the Hermes package's KWin build."
    notes+=("Detached Desktop/Steam requires /usr/bin/hermes-detached-session and KDE Plasma 6 Wayland.")
  fi
  info "Use Detached Desktop or Detached Steam in the app list, or choose session-type per app."

  if gamescope_unavailable; then
    warn "Ubuntu 24.04 cannot run the patched Gamescope. Detached Desktop/Steam remains an option on Plasma Wayland."
    return 0
  fi
  if ! ask "Also install support for isolated Gamescope application sessions?" n; then
    info "No Gamescope or seatd changes are needed for Plasma Detached Desktop/Steam."
    return 0
  fi

  explain <<'EOF'
Application isolation is a hardware-dependent experiment. It launches games
on private DRM cards and input seats, not inside the host desktop. This can
conflict with unsupported GPU drivers or other session management.
EOF
  case "$distro" in
    arch | ubuntu) pkg_install seatd pipewire pipewire-pulse wireplumber ;;
    fedora) pkg_install seatd pipewire pipewire-pulseaudio wireplumber ;;
  esac
  install_gamescope
  if [ "$did_gamescope" != 1 ]; then
    notes+=("Hermes Gamescope not installed; use Detached sessions only until the patched Gamescope is available.")
    return 0
  fi
  if ! install_kms_broker; then
    notes+=("Private-seat broker could not be configured; Gamescope application isolation is not ready.")
    return 0
  fi
  if ! verify_gamescope_binary; then
    notes+=("Gamescope isolation is not ready: the patched Gamescope binary did not pass verification.")
    return 0
  fi

  explain <<'EOF'
An optional private /dev hides host input controllers, keyboard and audio
devices from the remote game. It needs capabilities on the Hermes binary.
Review this permission carefully: cap_sys_admin is very powerful. Packages
may reset capabilities during an upgrade.
EOF
  if ask "Allow private /dev device isolation?" n; then
    if [ "$DRY_RUN" = 0 ] && ! command -v setcap >/dev/null 2>&1; then
      warn "setcap missing; install your distribution's libcap tools first."
    else
      as_root setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p /usr/bin/hermes
    fi
  fi
  did_isolated=1
  ok "Dedicated Gamescope session prerequisites configured. Choose session-type = application per app."
}

setup_prelogin() {
  step "Streaming the login screen (experimental)"
  explain <<'EOF'
Normally Hermes starts when you log in, so after a reboot nothing can connect
until someone logs in at the machine. With this, Hermes starts at boot and
streams the login screen: the client types the password and the stream carries
on into the desktop. It has been run on KDE Plasma with SDDM, Hermes-KMS and an
AMD GPU. It needs Hermes-KMS, your user in the input group, and a greeter that
lights a new display by itself (SDDM's Wayland greeter).
EOF
  if [ "$kms_ready" = 0 ]; then
    info "Skipped: streaming the login screen needs Hermes-KMS."
    return 0
  fi
  ask "Set up streaming the login screen?" n || return 0

  if [ "$DRY_RUN" = 0 ] && ! hermes --help 2>&1 | grep -q -- '--prelogin'; then
    warn "This Hermes build has no --prelogin command; install a newer one first."
    return 0
  fi
  run hermes --prelogin enable
  as_root usermod -aG input "$USER"
  need_reboot=1

  if systemctl is-enabled --quiet sddm 2>/dev/null; then
    explain <<'EOF'
SDDM is your display manager. Its X11 greeter never lights a virtual display,
so the login screen would be black. Hermes can add a drop-in that switches SDDM
to its Wayland greeter, /etc/sddm.conf.d/10-hermes-wayland.conf.
EOF
    if grep -Eqs '^[[:space:]]*DisplayServer[[:space:]]*=[[:space:]]*x11' /etc/sddm.conf; then
      warn "/etc/sddm.conf sets DisplayServer=x11, which overrides drop-ins; change it there."
    fi
    if grep -Eqs '^[[:space:]]*User[[:space:]]*=[[:space:]]*[^[:space:]]' /etc/sddm.conf /etc/sddm.conf.d/*.conf; then
      warn "SDDM has autologin set up; with autologin there is no login screen to stream."
    fi
    if ask "Switch SDDM to its Wayland greeter?" y; then
      local dropin="${workdir}/10-hermes-wayland.conf"
      cat >"$dropin" <<'EOF'
# Written by Hermes' installer: the Wayland greeter lights a virtual display
# by itself, so Hermes can stream the login screen. Remove this file to go
# back to SDDM's defaults.
[General]
DisplayServer=wayland
GreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell

[Wayland]
CompositorCommand=kwin_wayland --drm --no-lockscreen --no-global-shortcuts --locale1
EOF
      as_root install -Dm644 "$dropin" /etc/sddm.conf.d/10-hermes-wayland.conf
    fi
  else
    notes+=("Streaming the login screen has only been run with SDDM; check that your display manager's greeter lights a new display by itself.")
  fi
  did_prelogin=1
  ok "Hermes will start at boot. 'hermes --prelogin status' lists anything still in the way."
}

enable_service() {
  step "Hermes service"
  run systemctl --user daemon-reload
  if run systemctl --user enable --now hermes; then
    ok "hermes.service is enabled."
  else
    warn "Could not start hermes.service now; check systemctl --user status hermes."
    return 0
  fi
  if [ "$service_was_active" = 1 ] && [ "$DRY_RUN" = 0 ]; then
    explain <<'EOF'
Hermes was running before installation. The package or configuration may
have changed. Restarting Hermes disconnects active streams.
EOF
    if ask "Restart the existing Hermes service to apply changes?" n; then
      if run systemctl --user restart hermes; then
        ok "Hermes restarted with the installed binary and configuration."
      else
        warn "Hermes failed to restart; inspect systemctl --user status hermes."
      fi
    else
      notes+=("Hermes was active and was not restarted; restart it manually when no clients are connected.")
    fi
  fi
}


post_install_checks() {
  step "Post-install checks"
  if [ "$DRY_RUN" = 1 ]; then
    info "Dry run only: no packages, services or configuration were changed."
    return 0
  fi
  if command -v hermes >/dev/null 2>&1; then
    ok "Hermes executable found in PATH."
  else
    warn "Hermes executable missing from PATH."
  fi
  if systemctl --user is-active --quiet hermes; then
    ok "Hermes user service is active."
  else
    warn "Service not active: run systemctl --user status hermes."
  fi
  if [ "$kms_ready" = 1 ]; then
    verify_kms_installed || true
    kms_runtime_diagnostics
  fi
  if [ "$did_gamescope" = 1 ]; then
    verify_gamescope_binary || true
  fi
  if [ "$did_detached" = 1 ] && [ ! -x /usr/bin/hermes-detached-session ]; then
    warn "Detached Desktop helper is missing."
  fi
  for input_node in /dev/uinput /dev/uhid; do
    if [ ! -c "$input_node" ]; then
      warn "$input_node is missing; virtual input may need kernel/udev setup."
    elif [ ! -r "$input_node" ] || [ ! -w "$input_node" ]; then
      warn "$input_node is not readable/writable by this user; check input udev rules and ACLs."
    fi
  done
  info "Package checks cannot verify real GPU capture, input or private sessions."
  info "After a reboot, use the Hermes Audio/Video diagnostics and test a stream."
}

summary() {
  step "Done"
  if [ "$MODE" = update ]; then
    info "Update summary (1=updated; 0=unchanged/skipped): Hermes=$updated_hermes KMS=$updated_kms Gamescope=$updated_gamescope EVDI=$updated_evdi"
  fi
  info "Web UI: https://localhost:47990 (create a username and password on first visit)."
  [ "$kms_ready" = 1 ] && info "Hermes-KMS: available for this kernel."
  [ "$did_detached" = 1 ] && info "Detached Desktop/Steam: enabled (needs KDE Plasma 6 Wayland)."
  [ "$did_broker" = 1 ] && info "Private-seat brokers: configured."
  [ "$did_isolated" = 1 ] && info "Gamescope application isolation: configured; select session-type per app."
  [ "$did_gamescope" = 1 ] && info "Gamescope for Hermes: installed."
  [ "$did_prelogin" = 1 ] && info "Login screen streaming: enabled."
  local note
  for note in ${notes[@]+"${notes[@]}"}; do
    warn "$note"
  done
  if [ "$need_reboot" = 1 ]; then
    printf '\n    %sReboot before testing the new driver or private DRM seats.%s\n' "$bold" "$reset"
  fi
}

# Update mode only touches components already installed. It never changes the
# current virtual display backend, enables experimental modes or reconfigures
# pre-login streaming as part of a package update.
install_local_update_package() {
  case "$distro" in
    arch) as_root pacman -U --noconfirm "$@" ;;
    ubuntu) as_root apt-get install --reinstall -y "$@" ;;
    fedora) as_root dnf reinstall -y "$@" ;;
  esac
}

update_hermes() {
  step "Update Hermes host"
  local installed tag=latest pattern assets url candidate relation file digest
  installed=$(installed_package_version "$(hermes_package_name)" || true)
  [ -n "$installed" ] ||
    die "--update requires a package-managed Hermes installation; run without --update for initial setup."
  [ "$CHANNEL" = nightly ] && tag=nightly
  case "$distro" in
    arch) pattern='hermes-streaming-.*-x86_64[.]pkg[.]tar[.]zst$' ;;
    ubuntu) pattern="hermes_.*_ubuntu${distro_version}_amd64[.]deb$" ;;
    fedora) pattern="hermes-.*[.]fc${distro_version}[.]x86_64[.]rpm$" ;;
  esac
  assets=$(release_assets "$HERMES_REPO" "$tag") ||
    die "Cannot fetch Hermes $CHANNEL release metadata; installed package was preserved."
  url=$(release_asset_url "$assets" "$pattern")
  [ -n "$url" ] ||
    die "No $CHANNEL package for $distro $distro_version; leaving Hermes $installed installed."
  candidate=$(hermes_asset_version "$url") || die "Could not parse candidate Hermes version."
  relation=$(candidate_relation "$installed" "$candidate")
  info "Installed Hermes $installed; selected $CHANNEL candidate $candidate."
  if [ "$relation" = same ] && [ "$CHANNEL" = release ]; then
    ok "Already on the latest stable Hermes package."
    return 0
  fi
  if [ "$relation" = older ]; then
    warn "The selected $CHANNEL build is older than the installed package."
    ask "Explicitly downgrade Hermes $installed to $candidate?" n || return 0
  else
    ask "Update Hermes to $candidate ($CHANNEL)?" y || return 0
  fi
  file="${workdir}/${url##*/}"
  download "$url" "$file"
  if [ "$DRY_RUN" = 1 ]; then
    info "Dry-run: would inspect the asset and update Hermes if needed."
    return 0
  fi
  digest=$(sha256sum "$file")
  digest=${digest%% *}
  if [ "$relation" = same ] && cached_asset_matches "hermes-$CHANNEL" "$digest"; then
    ok "Hermes $CHANNEL artifact unchanged; no reinstall needed."
    return 0
  fi
  backup_config_for_update
  if [ "$relation" = same ]; then
    install_local_update_package "$file"
  else
    install_local_package "$file"
  fi
  updated_hermes=1
  remember_asset "hermes-$CHANNEL" "$digest"
  ok "Hermes updated. Existing user configuration was preserved."
}

# No KMS package is added during updates. A pinned 0.4.1 is never used to
# downgrade a newer driver just because Hermes itself was updated.
update_kms() {
  step "Update existing Hermes-KMS"
  local current core_version relation headers pattern broker_pattern assets url broker_url
  local core_file broker_file broker_version native_version src
  current=$(kms_installed_version || true)
  if [ -z "$current" ]; then
    info "Hermes-KMS is not installed; skipping optional driver."
    return 0
  fi
  core_version=$(kms_base_version "$current")
  relation=$(candidate_relation "$core_version" "${KMS_RELEASE#v}")
  if [ "$relation" = older ]; then
    warn "Hermes-KMS $current is newer than pinned ${KMS_RELEASE#v}; not downgrading."
    return 0
  fi
  if [ "$relation" = same ]; then
    if has_kms_installed; then
      kms_ready=1
      ok "Hermes-KMS $current is already installed for this kernel."
    else
      warn "Hermes-KMS exists, but its DKMS build is missing for $(uname -r)."
      notes+=("Run DKMS repair for Hermes-KMS after installing matching kernel headers.")
    fi
    return 0
  fi
  ask "Update installed Hermes-KMS $current to ${KMS_RELEASE#v}?" y || return 0
  native_version=$(installed_package_version "$(kms_core_package_name)" || true)
  broker_version=$(installed_package_version "$(kms_broker_package_name)" || true)
  headers=$(kernel_headers_package)
  case "$distro" in
    arch)
      pattern='/hermes-kms-dkms-git-.*-any[.]pkg[.]tar[.]zst$'
      broker_pattern='/hermes-kms-seatd-git-.*-any[.]pkg[.]tar[.]zst$'
      ;;
    ubuntu)
      pattern='/hermes-kms-dkms_.*_all[.]deb$'
      broker_pattern='/hermes-kms-seatd_.*_all[.]deb$'
      ;;
    fedora)
      pattern="/hermes-kms-dkms-.*[.]fc${distro_version}[.]noarch[.]rpm$"
      broker_pattern="/hermes-kms-seatd-.*[.]fc${distro_version}[.]noarch[.]rpm$"
      ;;
  esac
  assets=$(release_assets "$KMS_REPO" "$KMS_RELEASE") ||
    die "Cannot fetch pinned Hermes-KMS release metadata; old driver remains."
  url=$(release_asset_url "$assets" "$pattern")
  if [ -z "$url" ] && [ -n "$native_version" ]; then
    warn "No matching native KMS package. Refusing to overwrite a package-managed driver with a source install."
    return 0
  fi
  if [ -z "$url" ]; then
    warn "No matching native Hermes-KMS package for this distribution."
    ask "Replace the existing source-installed DKMS driver from pinned tag?" n || return 0
  fi
  if [ "$distro" = arch ]; then pkg_install base-devel dkms "$headers"
  elif [ "$distro" = ubuntu ]; then pkg_install build-essential dkms "$headers"
  else pkg_install make gcc dkms "$headers"; fi
  if [ "$DRY_RUN" = 0 ] && [ ! -f "/lib/modules/$(uname -r)/build/Makefile" ]; then
    warn "Matching headers for $(uname -r) unavailable; not replacing driver."
    return 0
  fi
  if [ -n "$url" ]; then
    broker_url=''
    if [ -n "$broker_version" ]; then
      broker_url=$(release_asset_url "$assets" "$broker_pattern")
      if [ -z "$broker_url" ]; then
        warn "Broker package for this OS missing; cannot safely update just the core."
        return 0
      fi
    fi
    core_file="${workdir}/${url##*/}"
    download_checked_kms "$assets" "$url" "$core_file"
    if [ -n "$broker_url" ]; then
      broker_file="${workdir}/${broker_url##*/}"
      download_checked_kms "$assets" "$broker_url" "$broker_file"
      install_local_package "$core_file" "$broker_file"
      did_broker=1
    else
      install_local_package "$core_file"
    fi
  else
    pkg_install git
    src="$workdir/Hermes-KMS"
    run git clone --depth 1 --branch "$KMS_RELEASE" "https://github.com/${KMS_REPO}.git" "$src"
    as_root make -C "$src" dkms-install
    if [ -x /usr/lib/hermes-kms/hermes-kms-setup ]; then
      as_root make -C "$src" install-broker-configs
    fi
  fi
  if verify_kms_installed; then
    kms_ready=1
    updated_kms=1
    need_reboot=1
    ok "Hermes-KMS updated; reboot before testing the module."
  else
    notes+=("Hermes-KMS update incomplete; inspect dkms status and kernel headers.")
  fi
}

update_gamescope() {
  step "Update existing Hermes Gamescope"
  local installed binary pattern assets url file digest
  installed=$(installed_package_version "$(gamescope_package_name)" || true)
  binary=$(gamescope_binary || true)
  if [ -z "$installed" ] && [ -z "$binary" ]; then
    info "Patched Gamescope is not installed; skipping optional component."
    return 0
  fi
  if [ -z "$installed" ]; then
    warn "The Gamescope installation is source-built rather than package-managed."
    if ask "Rebuild/migrate the existing Gamescope to the current Hermes fork?" n; then
      install_gamescope
      [ "$did_gamescope" = 1 ] && updated_gamescope=1
    fi
    return 0
  fi
  case "$distro" in
    arch) pattern='gamescope-hermes-.*-x86_64[.]pkg[.]tar[.]zst$' ;;
    ubuntu) pattern="gamescope-hermes_.*_ubuntu${distro_version}_amd64[.]deb$" ;;
    fedora) pattern="gamescope-hermes-.*[.]fc${distro_version}[.]x86_64[.]rpm$" ;;
  esac
  if ! assets=$(release_assets "$GAMESCOPE_REPO" "$GAMESCOPE_RELEASE"); then
    warn "Gamescope release unavailable. Existing package left intact."
    return 0
  fi
  url=$(release_asset_url "$assets" "$pattern")
  if [ -z "$url" ]; then
    warn "No matching patched Gamescope asset for $distro $distro_version."
    return 0
  fi
  ask "Check/update existing patched Gamescope ($installed)?" y || return 0
  file="${workdir}/${url##*/}"
  download "$url" "$file"
  if [ "$DRY_RUN" = 1 ]; then
    info "Dry-run: would compare Gamescope asset checksum."
    return 0
  fi
  digest=$(sha256sum "$file")
  digest=${digest%% *}
  if cached_asset_matches gamescope "$digest"; then
    did_gamescope=1
    ok "Gamescope rolling artifact unchanged."
    return 0
  fi
  install_local_update_package "$file"
  if verify_gamescope_binary; then
    did_gamescope=1
    updated_gamescope=1
    remember_asset gamescope "$digest"
    ok "Hermes Gamescope updated."
  else
    notes+=("Gamescope was updated, but the patched DRM interface could not be verified.")
  fi
}

update_evdi() {
  local package
  case "$distro" in
    arch) package=evdi ;;
    ubuntu) package=evdi-dkms ;;
    fedora) return 0 ;;
  esac
  [ -n "$(installed_package_version "$package" || true)" ] || return 0
  step "Optional EVDI update"
  ask "Update the already installed EVDI package?" n || return 0
  if [ "$distro" = ubuntu ]; then
    pkg_install evdi-dkms
  elif command -v paru >/dev/null 2>&1; then
    run paru -S --noconfirm evdi
  elif command -v yay >/dev/null 2>&1; then
    run yay -S --noconfirm evdi
  else
    warn "Cannot update AUR EVDI without a configured AUR helper."
    return 0
  fi
  updated_evdi=1
  need_reboot=1
}

finish_update_service() {
  [ "$updated_hermes" = 1 ] || return 0
  step "Hermes service after update"
  if [ "$service_was_active" = 0 ]; then
    info "Hermes was stopped; preserving its stopped/disabled state."
    return 0
  fi
  explain <<'EOF'
Hermes was updated but is still running its old executable.
Restarting disconnects clients, so it requires confirmation.
EOF
  if ask "Restart the existing Hermes service now?" n; then
    run systemctl --user daemon-reload
    if run systemctl --user restart hermes; then
      ok "Hermes restarted with the new executable."
    else
      warn "Hermes service could not be restarted; check its status."
    fi
  else
    notes+=("Hermes was updated but not restarted; restart it when no sessions are active.")
  fi
}

run_updater() {
  update_hermes
  update_kms
  update_gamescope
  update_evdi
  finish_update_service
  post_install_checks
  summary
}

main() {
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --update | --upgrade) MODE=update ;;
      --nightly) CHANNEL=nightly ;;
      --yes | -y) ASSUME_YES=1 ;;
      --dry-run) DRY_RUN=1 ;;
      -h | --help)
        usage
        exit 0
        ;;
      *)
        usage >&2
        exit 2
        ;;
    esac
    shift
  done

  printf '%sHermes installer / updater%s\n' "$bold" "$reset"
  [ "$DRY_RUN" = 1 ] && warn "Dry run: commands are shown, not run."
  preflight
  if [ "$MODE" = update ]; then
    run_updater
    return 0
  fi
  install_hermes
  install_kms
  install_evdi_if_needed
  install_isolated
  setup_prelogin
  enable_service
  post_install_checks
  summary
}

main "$@"
