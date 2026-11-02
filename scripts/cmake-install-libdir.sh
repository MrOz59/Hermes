#!/usr/bin/env bash
# CMake may preserve -D options as UNINITIALIZED rather than PATH.
set -euo pipefail
cache=${1:?usage: cmake-install-libdir.sh /path/to/CMakeCache.txt}
libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' "$cache")
if [[ -z $libdir || $libdir == *$'\n'* ]]; then
  echo "Cannot read one CMAKE_INSTALL_LIBDIR value from $cache" >&2
  exit 1
fi
printf '%s\n' "$libdir"
