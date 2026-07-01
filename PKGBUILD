# Maintainer: SudoMaker
# Apollo - Game streaming server with virtual display support

pkgname=apollo
pkgver=0.1.0
pkgrel=5
pkgdesc="Self-hosted game streaming server with virtual display support"
arch=('x86_64')
url='https://github.com/ClassicOldSong/Apollo'
license=('GPL-3.0-only')
install=apollo.install

depends=(
  'avahi'
  'curl'
  'libayatana-appindicator'
  'libcap'
  'libdrm'
  'libevdev'
  'libnotify'
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
  'udev'
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
  'gamescope: optional Gamescope Steam Session app'
  'kscreen: KDE Plasma Wayland virtual-display activation'
  'wl-clipboard: Hermes text clipboard synchronization on Wayland'
  'xclip: Hermes text clipboard synchronization on X11'
  'libva-mesa-driver: AMD GPU encoding support'
)

provides=('sunshine')
conflicts=('sunshine')

source=()
sha256sums=()

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
}

build() {
    cd "${startdir}"
    export BRANCH=local
    export BUILD_VERSION="${pkgver}"

    cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/apollo \
      -DSUNSHINE_ASSETS_DIR=share/apollo
    cmake --build build
}

package() {
    cd "${startdir}"
    DESTDIR="${pkgdir}" cmake --install build

    rm "${pkgdir}/usr/bin/sunshine"
    mv "${pkgdir}/usr/bin/sunshine-"* "${pkgdir}/usr/bin/apollo"
}
