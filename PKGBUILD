# Maintainer: SudoMaker
# Hermes - Game streaming server with virtual display support

pkgname=hermes-streaming
pkgver=0.6.0
pkgrel=1
pkgdesc="Self-hosted game streaming server with virtual display support"
arch=('x86_64')
url='https://github.com/MrOz59/Hermes'
license=('GPL-3.0-only')
install=hermes.install

depends=(
  'avahi'
  'curl'
  'libcap'
  'libdrm'
  'libevdev'
  'libpulse'
  'libva'
  'libx11'
  'libxcb'
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
)

makedepends=(
  'base-devel'
  'cmake'
  'git'
  'git-lfs'
  'nodejs'
  'npm'
)

optdepends=(
  'cuda: NVIDIA GPU encoding support'
  'evdi: Virtual display support for streaming to headless clients (AUR)'
  'gamescope: application-only experimental independent client sessions'
  'hermes-kms: zero-copy Hermes virtual displays and independent DRM devices'
  'kscreen: KDE Plasma Wayland virtual-display activation'
  'libva-mesa-driver: AMD GPU encoding support'
  'seatd: private seat brokers for experimental independent client sessions'
  'weston: experimental independent desktop client sessions'
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

# PyroWave (the pyrowave-shared library) is fetched and built from source at a
# pinned commit, so the encoder support (HAVE_PYROWAVE) is compiled in on any
# machine. The package ships the shared library itself, so no external PyroWave
# package is needed at runtime; the library loads Vulkan through volk, hence
# vulkan-icd-loader above. Bump the commit together with the API version the
# encoder code expects.
provides=('pyrowave-shared')
_pyrowave_commit=d2997ac172bdc00e29c58e3f2938acb7e94580bf

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

# Check out a single commit of a repository, without its history. A checkout
# already at that commit is reused, so rebuilding does not fetch again.
_fetch_commit() {
    local dir=$1 url=$2 commit=$3
    if [[ -d "${dir}/.git" && "$(git -C "${dir}" rev-parse HEAD)" == "${commit}" ]]; then
      return 0
    fi
    rm -rf "${dir}"
    git init -q "${dir}"
    git -C "${dir}" fetch -q --depth 1 "${url}" "${commit}"
    git -C "${dir}" checkout -q --detach FETCH_HEAD
}

prepare() {
    cd "${startdir}"
    # A source checkout already containing the submodules needs no mutation
    # of .git metadata (for example, when building in a restricted sandbox).
    if [[ ! -d third-party/moonlight-common-c/enet ]]; then
      git submodule update --init --recursive
    fi
    if command -v git-lfs >/dev/null 2>&1; then
      git lfs pull
    fi

    # The SDK is kept under build/, outside the Hermes sources.
    local sdk="${startdir}/build/pyrowave-sdk"
    _fetch_commit "${sdk}/src" https://github.com/Themaister/pyrowave.git \
      "${_pyrowave_commit}"
    # PyroWave pins the Granite commit it builds against in its own checkout
    # script, and needs only these two of Granite's submodules.
    local granite_commit
    granite_commit="$(sed -n 's/^GRANITE_COMMIT=//p' "${sdk}/src/checkout_granite.sh")"
    _fetch_commit "${sdk}/src/Granite" https://github.com/Themaister/Granite.git \
      "${granite_commit}"
    git -C "${sdk}/src/Granite" submodule update --init --depth 1 \
      third_party/volk third_party/khronos/vulkan-headers
}

build() {
    cd "${startdir}"
    # Nightly CI stamps the short commit into pkgver (0.5.1+abc1234) and passes
    # the matching BUILD_VERSION, so the binary reports the commit it was built
    # from. A plain local `makepkg` inherits neither and falls back to pkgver.
    export BRANCH="${BRANCH:-local}"
    export BUILD_VERSION="${BUILD_VERSION:-${pkgver}}"
    export COMMIT="${COMMIT:-$(git rev-parse HEAD)}"

    local sdk="${startdir}/build/pyrowave-sdk"
    cmake -S "${sdk}/src" -B "${sdk}/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${sdk}/install" \
      -DCMAKE_INSTALL_LIBDIR=lib
    cmake --build "${sdk}/build"
    cmake --install "${sdk}/build"
    export PKG_CONFIG_PATH="${sdk}/install/share/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    # CMake looks pyrowave-shared up quietly and leaves the codec out when it is
    # missing; fail here rather than ship a package without it.
    pkg-config --exists pyrowave-shared

    # CUDA is required so the package ships NVIDIA NVENC hardware encoding. The
    # build fails if the CUDA toolkit is missing (CUDA_FAIL_ON_MISSING defaults
    # to ON) — install the `cuda` package (a makedepend) before building.
    cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/hermes \
      -DSUNSHINE_ASSETS_DIR=share/hermes
    cmake --build build
}

package() {
    cd "${startdir}"
    DESTDIR="${pkgdir}" cmake --install build

    rm "${pkgdir}/usr/bin/sunshine"
    mv "${pkgdir}/usr/bin/sunshine-"* "${pkgdir}/usr/bin/hermes"

    # Ship the PyroWave library the hermes binary links against, with its
    # soname symlinks.
    install -d "${pkgdir}/usr/lib"
    cp -a "${startdir}/build/pyrowave-sdk/install/lib/"libpyrowave-shared.so* \
      "${pkgdir}/usr/lib/"

    # The systemd unit generated by CMake is named sunshine.service; rename it
    # to hermes.service so it does not collide with an apollo/sunshine install.
    if [[ -f "${pkgdir}/usr/lib/systemd/user/sunshine.service" ]]; then
      mv "${pkgdir}/usr/lib/systemd/user/sunshine.service" \
         "${pkgdir}/usr/lib/systemd/user/hermes.service"
    fi
}
