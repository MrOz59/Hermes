#!/usr/bin/env bash
# Exercise the production CMake install rule without building the whole host/SDK.
# Needs root, a C compiler, CMake, readelf, setcap and runuser. Never skip AT_SECURE.
set -euo pipefail
[[ $EUID == 0 ]]
layout_only=false
if [[ ${1:-} == --layout-only ]]; then
  layout_only=true
  echo 'Layout-only mode: secure-execution checks are NOT run.'
fi
run_unprivileged() {
  if $layout_only; then
    env -i PATH=/usr/bin:/bin "$@"
  else
    runuser -u nobody -- env -i PATH=/usr/bin:/bin "$@"
  fi
}
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d /tmp/hermes-runtime-loader.XXXXXX)
trap 'rm -rf "$work"' EXIT
chmod 755 "$work"
# Cache types differ between Ubuntu's CMake versions. Exercise every type
# observed for this command-line option, including 26.04's UNINITIALIZED.
for cache_type in PATH STRING UNINITIALIZED; do
  printf 'CMAKE_INSTALL_LIBDIR:%s=lib/x86_64-linux-gnu\n' "$cache_type" > "$work/cache-fixture"
  [[ $(bash "$repo/scripts/cmake-install-libdir.sh" "$work/cache-fixture") == lib/x86_64-linux-gnu ]]
done
printf 'CMAKE_INSTALL_BINDIR:PATH=bin\n' > "$work/cache-fixture"
if bash "$repo/scripts/cmake-install-libdir.sh" "$work/cache-fixture" 2>/dev/null; then
  echo 'Missing LIBDIR was incorrectly accepted' >&2
  exit 1
fi
mkdir -p "$work/sdk/lib" "$work/source"
printf 'int codec_fixture(void) { return 42; }\n' > "$work/codec.c"
cc -shared -fPIC "$work/codec.c" -Wl,-soname,libpyrowave-shared.so.0 \
  -o "$work/sdk/lib/libpyrowave-shared.so.0.5.0"
ln -s libpyrowave-shared.so.0.5.0 "$work/sdk/lib/libpyrowave-shared.so.0"
ln -s libpyrowave-shared.so.0 "$work/sdk/lib/libpyrowave-shared.so"
cat > "$work/source/main.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <unistd.h>
extern int codec_fixture(void);
int main(int argc, char **argv) {
    unsigned long secure = getauxval(AT_SECURE);
    printf("uid=%lu AT_SECURE=%lu codec=%d\n", (unsigned long)getuid(), secure, codec_fixture());
    return argc != 2 || (getuid() == 0 && secure != 0) || secure != strtoul(argv[1], NULL, 10) || codec_fixture() != 42;
}
C
cat > "$work/source/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(runtime_fixture C)
include(GNUInstallDirs)
# Only SDK discovery is stubbed; installation and RPATH use production code.
function(pkg_get_variable output package variable)
    if(variable STREQUAL "libdir")
        set(${output} "${SDK}/lib" PARENT_SCOPE)
    elseif(variable STREQUAL "prefix")
        set(${output} "${SDK}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unexpected pkg-config variable")
    endif()
endfunction()
add_executable(sunshine main.c)
target_link_libraries(sunshine PRIVATE "${SDK}/lib/libpyrowave-shared.so")
include("${HERMES_SOURCE}/cmake/targets/pyrowave-runtime.cmake")
sunshine_bundle_pyrowave_runtime(sunshine)
install(TARGETS sunshine DESTINATION bin)
CMAKE
for libdir in lib lib64 lib/x86_64-linux-gnu; do
  prefix="$work/install-${libdir//\//-}"
  cmake -S "$work/source" -B "$work/build-${libdir//\//-}" \
    -DHERMES_SOURCE="$repo" -DSDK="$work/sdk" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR="$libdir"
  [[ $(bash "$repo/scripts/cmake-install-libdir.sh" "$work/build-${libdir//\//-}/CMakeCache.txt") == "$libdir" ]]
  cmake --build "$work/build-${libdir//\//-}"
  DESTDIR="$work/stage" cmake --install "$work/build-${libdir//\//-}"
  # DESTDIR must not enter the runtime path. Move to the configured prefix.
  mv "$work/stage$prefix" "$prefix"
  test -f "$prefix/$libdir/hermes/libpyrowave-shared.so.0"
  runpath=$(readelf -d "$prefix/bin/sunshine" | sed -n 's/.*(RUNPATH).*\[\(.*\)\].*/\1/p')
  [[ $runpath == "$prefix/$libdir/hermes" ]]
  run_unprivileged "$prefix/bin/sunshine" 0
  if $layout_only; then continue; fi
  setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p "$prefix/bin/sunshine"
  runuser -u nobody -- env -i PATH=/usr/bin:/bin "$prefix/bin/sunshine" 1

  # Negative control: the original ORIGIN rule loads normally, but is rejected
  # with file capabilities outside the loader's trusted system directories.
  cc "$work/source/main.c" -L"$prefix/$libdir/hermes" -lpyrowave-shared \
    -Wl,-rpath,"\$ORIGIN/../$libdir/hermes" -o "$prefix/bin/old-origin"
  runuser -u nobody -- env -i PATH=/usr/bin:/bin "$prefix/bin/old-origin" 0
  setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p "$prefix/bin/old-origin"
  status=0
  runuser -u nobody -- env -i PATH=/usr/bin:/bin "$prefix/bin/old-origin" 1 \
    > "$work/negative.log" 2>&1 || status=$?
  cat "$work/negative.log"
  [[ $status == 127 ]]
  grep -F 'libpyrowave-shared.so.0' "$work/negative.log"
done

# Portable bundles must still work after their prefix moves (without caps).
cmake -S "$work/source" -B "$work/portable-build" -DHERMES_SOURCE="$repo" \
  -DSDK="$work/sdk" -DCMAKE_INSTALL_PREFIX="$work/portable" \
  -DCMAKE_INSTALL_LIBDIR=lib -DSUNSHINE_BUILD_APPIMAGE=ON
cmake --build "$work/portable-build"
cmake --install "$work/portable-build"
mv "$work/portable" "$work/moved"
run_unprivileged "$work/moved/bin/sunshine" 0
if $layout_only; then
  echo 'Native layout and portable relocation passed; AT_SECURE remains untested.'
else
  echo 'Native secure loader, negative controls and portable relocation passed.'
fi
