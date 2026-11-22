# Maintainer: SudoMaker
# Hermes - Game streaming server with virtual display support

pkgname=hermes-streaming
pkgver=0.7.0
pkgrel=1
pkgdesc="Self-hosted game streaming server with virtual display support"
arch=('x86_64')
url='https://github.com/MrOz59/Hermes'
license=('GPL-3.0-only')
install=hermes.install

depends=(
  'acl'
  'avahi'
  'curl'
  'libcap'
  'libdrm'
  'libevdev'
  'libpipewire'
  'libpulse'
  'libva'
  'libx11'
  'libxcb'
  'libxext'
  'libxfixes'
  'libxrandr'
  'libxtst'
  'miniupnpc'
  'numactl'
  'openssl'
  'opus'
  'qt6-base'
  'qt6-svg'
  'udev'
  'vulkan-icd-loader'
  'wayland'
)

makedepends=(
  'base-devel'
  'git'
  'git-lfs'
  'jq'
  'ninja'
  'nlohmann-json'
  'bun>=1.4.5'
  'patchelf'
  'plasma-wayland-protocols'
  'python'
  'vulkan-headers'
)

optdepends=(
  'cuda: NVIDIA GPU encoding support'
  'evdi: Virtual display support for streaming to headless clients (AUR)'
  'gamescope: application-only experimental independent client sessions'
  'hermes-kms: zero-copy Hermes virtual displays and independent DRM devices'
  'kscreen: KDE Plasma Wayland virtual-display activation'
  'libva-mesa-driver: AMD GPU encoding support'
  'pipewire: private audio server per independent client session'
  'pipewire-pulse: PulseAudio clients in independent client sessions'
  'seatd: private seat brokers for experimental independent client sessions'
  'weston: experimental independent desktop client sessions'
  'wireplumber: links streams in the private audio server of independent client sessions'
  'wl-clipboard: Hermes text clipboard synchronization on Wayland'
  'xclip: Hermes text clipboard synchronization on X11'
)

# The Arch package was originally published as `hermes`, but that name belongs
# to an unrelated AUR PAM authentication project. Prevent both packages from
# owning the same files, while automatically replacing only the old Hermes
# streaming packages whose versions were below 0.5.0.
conflicts=('hermes')
replaces=('hermes<0.5.0')

# Hermes installs under its own /usr/bin/hermes and /usr/share/hermes paths and
# ships hermes.service, so it can be installed side by side with the apollo (AUR)
# and sunshine packages — useful while the project is experimental. No provides
# or conflicts are declared for those packages; the binaries and unit names are
# all distinct.

# The native PyroWave SDK script pins the source revision. The package ships
# its shared library and uses the system Vulkan loader.
# Keep the separate hermes-streaming-debug package. The main binary is still
# stripped, so nothing about the shipped package changes; what changes is that
# the symbols exist somewhere. Without them a user's crash report is a list of
# raw offsets into a stripped binary, which is exactly where the first two
# SIGSEGV reports stopped: the stack was there, and nothing could be said about
# what was on it. A debug package the reporter can install alongside turns
# `coredumpctl gdb` into an answer.
#
# Nothing is emitted unless the build environment asks for it: this relies on
# `debug` being in makepkg.conf's OPTIONS, which the CI container has and a
# plain local `makepkg` usually does not.

source=()
sha256sums=()

_mcpp_environment() {
    export MCPP_HOME="${startdir}/.mcpp/arch-package"
    export MCPP_TOOLCHAIN="path:${startdir}/.toolchain/arch-compiler"
    export HERMES_MCPP_QT_MOC="${HERMES_MCPP_QT_MOC:-/usr/lib/qt6/moc}"
    export PATH="${MCPP_HOME}/bin:${PATH}"
    if [[ -z ${XDG_RUNTIME_DIR:-} ]]; then
      export XDG_RUNTIME_DIR="${MCPP_HOME}/runtime"
      install -d -m 700 "${XDG_RUNTIME_DIR}"
    fi
}

prepare() {
    cd "${startdir}"
    if [[ ! -f third-party/moonlight-common-c/enet/include/enet/enet.h ]]; then
      git submodule update --init --recursive
    fi
    git lfs pull

    _mcpp_environment
    scripts/prepare-mcpp-ci.sh
    scripts/prepare-mcpp-linux-sdk.sh
    bun install --frozen-lockfile
}

build() {
    cd "${startdir}"
    _mcpp_environment
    export BRANCH="${BRANCH:-local}"
    export BUILD_VERSION="${BUILD_VERSION:-${pkgver}}"
    export COMMIT="${COMMIT:-$(git rev-parse HEAD)}"

    local sdk="${srcdir}/hermes-pyrowave-sdk"
    scripts/build-pyrowave-sdk.sh "${sdk}"
    export HERMES_MCPP_PYROWAVE_PREFIX="${sdk}/install"
    export HERMES_MCPP_PYROWAVE_RUNPATH=/usr/lib/hermes

    # Release and CI packages require CUDA. A local build uses the SDK if present.
    local features=kwin,package-assets,pyrowave,tray,xtest
    local cuda_prefix="${HERMES_MCPP_CUDA_PREFIX:-}" nvcc
    if [[ -z ${cuda_prefix} ]]; then
      if nvcc="$(command -v nvcc)"; then
        cuda_prefix="$(dirname "$(dirname "$(readlink -f "${nvcc}")")")"
      elif [[ -x ${startdir}/.toolchain/cuda-12.9/bin/nvcc ]]; then
        cuda_prefix="${startdir}/.toolchain/cuda-12.9"
      elif [[ -x /opt/cuda/bin/nvcc ]]; then
        cuda_prefix=/opt/cuda
      fi
    fi
    if [[ -n ${cuda_prefix} ]]; then
      export HERMES_MCPP_CUDA_PREFIX="${cuda_prefix}"
      if [[ -z ${CUDAHOSTCXX:-} ]]; then
        export CUDAHOSTCXX="$(command -v g++-14 || command -v g++)"
      fi
      features+=,cuda
    elif [[ ${BRANCH} != local ]]; then
      printf '%s\n' 'The Arch release package requires a CUDA toolkit.' >&2
      return 1
    fi

    mcpp build --offline --profile arch-package -p hermes-host --features "${features}"
    jq -er '.data.result' target/.last-result.json > "${srcdir}/hermes-host-result.path"

    mcpp build --offline --profile arch-package -p hermes-kms-card-broker \
      -p hermes-session-broker -p hermes-detached-session -p hermes-gamemode
    jq -er '.data.result' target/.last-result.json > "${srcdir}/hermes-tools-result.path"
}

package() {
    cd "${startdir}"
    local staging
    staging="$(mktemp -d "${srcdir}/hermes-payload.XXXXXX")"
    (
      trap 'rm -rf -- "${staging}"' EXIT
      packaging/linux/stage-install.sh --mcpp "${staging}/payload" \
        --result "$(<"${srcdir}/hermes-host-result.path")" \
        --result "$(<"${srcdir}/hermes-tools-result.path")" \
        --pyrowave-prefix "${srcdir}/hermes-pyrowave-sdk/install" --libdir lib --tray
      cp -a "${staging}/payload/." "${pkgdir}/"
    )
}
