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

Installs Hermes from its GitHub releases and asks about each optional piece.

Options:
  --nightly   install the rolling nightly build of main instead of the
              latest release
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
set_config() {
  local key=$1 value=$2 file
  file=$(config_file)
  printf '    %s%s: %s = %s%s\n' "$dim" "$file" "$key" "$value" "$reset"
  [ "$DRY_RUN" = 1 ] && return
  mkdir -p "$(dirname "$file")"
  touch "$file"
  if grep -Eq "^[[:space:]]*${key}[[:space:]]*=" "$file"; then
    sed -i -E "s|^[[:space:]]*${key}[[:space:]]*=.*|${key} = ${value}|" "$file"
  else
    printf '%s = %s\n' "$key" "$value" >>"$file"
  fi
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

has_kms_installed() {
  [ "$DRY_RUN" = 0 ] || return 1
  command -v dkms >/dev/null 2>&1 || return 1
  dkms status -m hermes-kms 2>/dev/null | grep -F "$(uname -r)" | grep -q installed
}

verify_kms_installed() {
  [ "$DRY_RUN" = 1 ] && return 0
  if ! has_kms_installed; then
    warn "DKMS does not report hermes-kms installed for $(uname -r)."
    warn "Inspect: dkms status -m hermes-kms"
    return 1
  fi
  ok "Hermes-KMS DKMS installed for $(uname -r)."
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
      warn "No release package for ${distro} ${distro_version}; trying nightly."
      assets=$(release_assets "$HERMES_REPO" nightly) || die "could not read the nightly release from GitHub."
      url=$(printf '%s\n' "$assets" | grep -E "$pattern" | head -n1 || true)
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
    as_root make -C "$src" INSTALL_BROKER=1 dkms-install
    as_root make -C "$src" install-configs
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
  if [ "$broker_from_source" = 0 ]; then
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
  did_gamescope=1
  ok "Gamescope for Hermes installed in ${GAMESCOPE_SOURCE_PREFIX}."
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
  did_detached=1
  ok "Independent-session capability enabled; shared streaming remains available."
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
  if ! install_kms_broker; then
    notes+=("Private-seat broker could not be configured; Gamescope application isolation is not ready.")
    return 0
  fi
  install_gamescope
  if [ "$did_gamescope" != 1 ]; then
    notes+=("Hermes Gamescope not installed; use Detached sessions only until the patched Gamescope is available.")
    return 0
  fi
  if [ "$DRY_RUN" = 0 ] &&
     [ ! -x /usr/lib/hermes-gamescope/bin/gamescope ] &&
     [ ! -x /opt/hermes-gamescope/bin/gamescope ]; then
    warn "Patched Gamescope executable was not found after install."
    notes+=("Gamescope application isolation not ready: verify the installed Gamescope binary.")
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
    warn "Could not start hermes.service now; it starts with your next login. Check: systemctl --user status hermes"
  fi
}

summary() {
  step "Done"
  info "Web UI: https://localhost:47990 (set a username and password on first visit)."
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
    printf '\n    %sReboot to load the driver and the new group memberships.%s\n' "$bold" "$reset"
  fi
}

main() {
  while [ "$#" -gt 0 ]; do
    case "$1" in
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

  printf '%sHermes installer%s\n' "$bold" "$reset"
  [ "$DRY_RUN" = 1 ] && warn "Dry run: commands are shown, not run."
  preflight
  install_hermes
  install_kms
  install_isolated
  setup_prelogin
  enable_service
  summary
}

main "$@"
