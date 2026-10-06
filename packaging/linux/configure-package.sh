#!/usr/bin/env bash

# Keep the DEB and RPM builds on the same CMake feature set. Distribution jobs
# should differ only in their compiler/libraries and final package format.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-build}"
if [[ $# -gt 0 ]]; then
  shift
fi

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ASSETS_DIR=share/hermes \
  -DSUNSHINE_ENABLE_WAYLAND=ON \
  -DSUNSHINE_ENABLE_X11=ON \
  -DSUNSHINE_ENABLE_DRM=ON \
  -DCUDA_FAIL_ON_MISSING=ON \
  "$@"
