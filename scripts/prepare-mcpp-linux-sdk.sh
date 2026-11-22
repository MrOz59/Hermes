#!/usr/bin/env bash
# Prepare the native dependency layout in a fresh Linux CI checkout.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$repo_root/.toolchain"
jobs="${MCPP_JOBS:-$(nproc)}"
include_dir="$(pkg-config --variable=includedir libdrm)"
lib_dir="$(pkg-config --variable=libdir libdrm)"
test -d "$include_dir"
test -d "$lib_dir"
test -f "$include_dir/nlohmann/json.hpp"

# These paths also accept a locally extracted SDK on developer workstations.
native="$sdk/fedora/usr"
for directory in "$native/include" "$sdk"/{acl,linux-headers-7.3.5,json-include,qt6/include,boost,libvirtualhid}; do
    if [[ $(realpath -m -- "$directory") != "$directory" ]]; then
        printf 'Expected an SDK directory inside this checkout: %s\n' "$directory" >&2
        exit 1
    fi
done
mkdir -p "$native/include" "$sdk"/{acl,linux-headers-7.3.5,json-include}
ln -sfnT "$lib_dir" "$native/lib64"
ln -sfnT /usr/bin "$native/bin"
for name in libdrm opus pipewire-0.3 spa-0.2 libevdev-1.0 SDL2; do
    if [[ -d $include_dir/$name ]]; then
        ln -sfnT "$include_dir/$name" "$native/include/$name"
    fi
done
ln -sfnT "$include_dir" "$sdk/linux-headers-7.3.5/include"
ln -sfnT "$include_dir" "$sdk/acl/include"
ln -sfnT "$lib_dir" "$sdk/acl/lib"
ln -sfnT "$include_dir/nlohmann" "$sdk/json-include/nlohmann"

if pkg-config --exists Qt6Core; then
    qt_include="$(pkg-config --variable=includedir Qt6Core)"
    test -d "$qt_include/QtCore"
    mkdir -p "$sdk/qt6/include"
    ln -sfnT "$qt_include" "$native/include/qt6"
    ln -sfnT "$qt_include" "$sdk/qt6/include/qt6"
    ln -sfnT "$lib_dir" "$sdk/qt6/lib64"
fi

boost_archive="$sdk/boost_1_92_0.tar.bz2"
boost_hash=5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c
if [[ ! -f $boost_archive ]]; then
    download="$(mktemp "$boost_archive.XXXXXX")"
    trap 'rm -f "$download"' EXIT
    curl -fLsS --retry 3 \
        https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2 -o "$download"
    printf '%s  %s\n' "$boost_hash" "$download" | sha256sum --check
    mv -T "$download" "$boost_archive"
fi
printf '%s  %s\n' "$boost_hash" "$boost_archive" | sha256sum --check
if ! grep -qx '#define BOOST_VERSION 109200' "$sdk/boost/boost/version.hpp" 2>/dev/null; then
    mkdir -p "$sdk/boost"
    tar -xjf "$boost_archive" --strip-components=1 -C "$sdk/boost"
fi
(
    cd "$sdk/boost"
    if [[ ${MCPP_TOOLCHAIN:-} == path:/* ]]; then
        compiler_bin="${MCPP_TOOLCHAIN#path:}/bin"
        test -x "$compiler_bin/g++"
        export PATH="$compiler_bin:$PATH"
    fi
    if [[ ! -x b2 ]]; then
        ./bootstrap.sh
    fi
    ./b2 -j"$jobs" variant=release link=static runtime-link=shared threading=multi \
        boost.locale.icu=off boost.locale.iconv=on \
        --with-atomic --with-chrono --with-container --with-context --with-date_time \
        --with-filesystem --with-locale --with-log --with-process --with-program_options \
        --with-regex --with-serialization --with-thread --with-charconv stage
)

virtualhid="$sdk/libvirtualhid"
revision=14ee6c4717d32b678c2983bfa6c54019b944d421
if [[ $(git -C "$virtualhid" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
    git init -q "$virtualhid"
    git -C "$virtualhid" fetch --depth 1 https://github.com/MrOz59/libvirtualhid.git "$revision"
    git -C "$virtualhid" checkout --detach "$revision"
fi
