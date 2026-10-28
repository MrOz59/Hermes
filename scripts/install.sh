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
GAMESCOPE_REPO="MrOz59/gamescope"
GAMESCOPE_BRANCH="hermes"
GAMESCOPE_RELEASE="hermes-nightly"
GAMESCOPE_SOURCE_PREFIX="/opt/hermes-gamescope"

ASSUME_YES=0
DRY_RUN=0
CHANNEL="release"

# What was decided, for the summary and for steps that depend on each other.
did_kms=0
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
Hermes-KMS is a kernel module (built with DKMS) that gives Hermes virtual
displays it can capture without copying, one per client. Isolated sessions
and streaming the login screen need it; the EVDI driver Hermes otherwise uses
supports neither. It is built from the driver's main branch, so this needs
your kernel's headers and a compiler, and it is rebuilt for each new kernel.
EOF
  ask "Install Hermes-KMS?" y || return 0

  local headers src
  headers=$(kernel_headers_package)
  case "$distro" in
    arch) pkg_install base-devel git dkms "$headers" ;;
    ubuntu) pkg_install build-essential git dkms "$headers" ;;
    fedora) pkg_install make gcc git dkms "$headers" ;;
  esac

  src="${workdir}/Hermes-KMS"
  run git clone --depth 1 "https://github.com/${KMS_REPO}.git" "$src"
  if [ "$distro" = arch ]; then
    # The package splits into the DKMS driver and the private-seat brokers,
    # with the boot drop-ins included.
    if [ "$DRY_RUN" = 0 ]; then
      (cd "$src" && run makepkg -si --noconfirm --needed)
    else
      run makepkg -si --noconfirm --needed
    fi
  else
    as_root make -C "$src" dkms-install
    as_root make -C "$src" install-configs
  fi
  did_kms=1
  need_reboot=1

  info "Giving your user the driver's session cards (hermes-kms-setup)."
  as_root /usr/lib/hermes-kms/hermes-kms-setup configure --user "$(id -u)"

  explain <<'EOF'
Hermes uses EVDI for its virtual display unless told otherwise. Hermes-KMS is
still opt-in, and some Hermes features only work with it.
EOF
  if ask "Make Hermes-KMS Hermes' virtual display backend?" y; then
    set_config virtual_display_backend hermes_kms
  else
    notes+=("Hermes-KMS is installed but not selected: set virtual_display_backend = hermes_kms in $(config_file) to use it.")
  fi
  ok "Hermes-KMS installed."
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
  step "Isolated sessions (experimental)"
  if gamescope_unavailable; then
    warn "Isolated sessions through Gamescope are unavailable on Ubuntu 24.04 for now."
    explain <<'EOF'
The Gamescope they run in needs newer Wayland, libdrm and xkbcommon libraries
than Ubuntu 24.04 ships, so Hermes' Gamescope build is not packaged for it.
They work on Ubuntu 26.04, Fedora 43/44, Arch and CachyOS. The rest of Hermes
works on 24.04 as usual.
EOF
    notes+=("Isolated sessions through Gamescope are unavailable on Ubuntu 24.04 for now.")
    return 0
  fi
  if [ "$did_kms" = 0 ]; then
    info "Skipped: isolated sessions need Hermes-KMS."
    return 0
  fi
  explain <<'EOF'
An isolated session runs the game in its own Gamescope, on its own virtual
display and input seat, as your user. The desktop stays usable at the same
time, and neither sees the other's input, audio or controllers: each session
gets a private audio server (PipeWire), and can get a private /dev holding
only its client's devices. This is experimental and changes as it is tested.
EOF
  ask "Set up isolated sessions?" n || return 0

  case "$distro" in
    arch) pkg_install seatd pipewire pipewire-pulse wireplumber ;;
    ubuntu) pkg_install seatd pipewire pipewire-pulse wireplumber ;;
    fedora) pkg_install seatd pipewire pipewire-pulseaudio wireplumber ;;
  esac
  if getent group seat >/dev/null; then
    as_root usermod -aG seat "$USER"
    need_reboot=1
  fi

  install_gamescope

  explain <<'EOF'
A private /dev hides the host's controllers, keyboards and other devices from
the session, and the session's virtual devices from the desktop. Hermes needs
three file capabilities on its binary for it (cap_sys_admin, which it already
has for capture, plus cap_mknod and cap_sys_chroot). Without them sessions
still run, but see the host's /dev.
EOF
  if ask "Give Hermes the capabilities for a private /dev?" y; then
    as_root setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p /usr/bin/hermes
    notes+=("Reinstalling or upgrading Hermes resets its file capabilities; run this installer again or setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p /usr/bin/hermes.")
  fi

  set_config hermes_kms_isolated_sessions enabled
  did_isolated=1
  ok "Isolated sessions enabled."
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
  if [ "$did_kms" = 0 ]; then
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
  [ "$did_kms" = 1 ] && info "Hermes-KMS: installed."
  [ "$did_isolated" = 1 ] && info "Isolated sessions: enabled; choose a session type per application in the Web UI."
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
