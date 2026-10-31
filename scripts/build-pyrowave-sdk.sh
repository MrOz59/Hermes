#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${1:-${repo_root}/build/pyrowave-sdk}"
revision=d2997ac172bdc00e29c58e3f2938acb7e94580bf

fetch_commit() {
    local path=$1 url=$2 commit=$3
    [[ -d "${path}/.git" ]] || git init -q "$path"
    if [[ "$(git -C "$path" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]]; then
        git -C "$path" fetch --depth 1 "$url" "$commit"
        git -C "$path" checkout --detach FETCH_HEAD
    fi
}
mkdir -p "$sdk"
fetch_commit "$sdk/src" https://github.com/Themaister/pyrowave.git "$revision"
granite="$(sed -n 's/^GRANITE_COMMIT=//p' "$sdk/src/checkout_granite.sh")"
[[ "$granite" =~ ^[a-f0-9]{40}$ ]]
fetch_commit "$sdk/src/Granite" https://github.com/Themaister/Granite.git "$granite"
git -C "$sdk/src/Granite" submodule update --init --depth 1 \
    third_party/volk third_party/khronos/vulkan-headers
cmake -S "$sdk/src" -B "$sdk/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sdk/install" \
    -DCMAKE_INSTALL_LIBDIR=lib -DPYROWAVE_DEVEL=OFF -DPYROWAVE_UTILS=OFF
cmake --build "$sdk/build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
cmake --install "$sdk/build"
mkdir -p "$sdk/install/share/licenses/pyrowave"
cp "$sdk/src/LICENSE" "$sdk/install/share/licenses/pyrowave/LICENSE"
cp "$sdk/src/Granite/LICENSE" "$sdk/install/share/licenses/pyrowave/Granite-LICENSE"
for license in "$sdk/src/Granite/third_party/volk"/LICENSE*; do
    [[ -f "$license" ]] || continue
    cp "$license" "$sdk/install/share/licenses/pyrowave/volk-LICENSE"
done
pkg_path="$sdk/install/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
PKG_CONFIG_PATH="$pkg_path" pkg-config --atleast-version=0.5 pyrowave-shared
if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "PKG_CONFIG_PATH=$pkg_path" >> "$GITHUB_ENV"
    echo "LD_LIBRARY_PATH=$sdk/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" >> "$GITHUB_ENV"
fi
echo "PyroWave $revision installed in $sdk/install"
