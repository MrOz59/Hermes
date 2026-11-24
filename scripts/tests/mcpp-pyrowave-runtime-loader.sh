#!/usr/bin/env bash
# Exercise the mcpp package stager with a small ELF and an SDK-shaped library.
set -euo pipefail
export LC_ALL=C
layout_only=false
case ${1:-} in
  --layout-only) layout_only=true ;;
  '') [[ $EUID == 0 ]] ;;
  *) printf 'usage: %s [--layout-only]\n' "$0" >&2; exit 2 ;;
esac

repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/hermes-mcpp-loader.XXXXXX")
secure_libdir=
cleanup() {
  if [[ -n "$secure_libdir" ]]; then rm -rf -- "$secure_libdir"; fi
  rm -rf -- "$work"
}
trap cleanup EXIT
chmod 755 "$work"
sdk="$work/sdk with spaces"
mkdir -p "$work"/{pkgconfig,assets/web,assets/session-compositors} "$sdk"/{lib,share/licenses/pyrowave}
printf '[]\n' > "$work/assets/apps.json"
printf '<!doctype html>\n' > "$work/assets/web/index.html"
touch "$work/assets/session-compositors/weston.conf"
printf 'fixture license\n' > "$sdk/share/licenses/pyrowave/LICENSE"

cat > "$work/pkgconfig/systemd.pc" <<'PC'
systemd_user_unit_dir=/usr/lib/systemd/user
systemd_system_unit_dir=/usr/lib/systemd/system
sysusers_dir=/usr/lib/sysusers.d
modules_load_dir=/usr/lib/modules-load.d
Name: systemd
Description: Fixture paths
Version: 1
PC
cat > "$work/pkgconfig/udev.pc" <<'PC'
udev_dir=/usr/lib/udev
Name: udev
Description: Fixture paths
Version: 1
PC
export PKG_CONFIG_LIBDIR="$work/pkgconfig" PKG_CONFIG_PATH=

printf 'int codec_fixture(void) { return 42; }\n' > "$work/codec.c"
cc -shared -fPIC "$work/codec.c" -Wl,-soname,libpyrowave-shared.so.0 \
  -o "$sdk/lib/libpyrowave-shared.so.0.5.0"
ln -s libpyrowave-shared.so.0.5.0 "$sdk/lib/libpyrowave-shared.so.0"
ln -s libpyrowave-shared.so.0 "$sdk/lib/libpyrowave-shared.so"
hash=$(sha256sum < "$sdk/lib/libpyrowave-shared.so.0")
printf '#define PROJECT_VERSION "2026.701.0"\n#define HERMES_BUILD_PYROWAVE_PREFIX %s\n#define HERMES_BUILD_PYROWAVE_SHA256 "%s"\n' \
  "$(jq -cn --arg sdk "$sdk" '$sdk')" "${hash%% *}" > "$work/hermes-build-version.h"
cat > "$work/host.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <unistd.h>

extern int codec_fixture(void);

int main(int argc, char **argv) {
    unsigned long secure = getauxval(AT_SECURE);
    int value = codec_fixture();
    puts("/usr/share/hermes");
    printf("uid=%lu AT_SECURE=%lu codec=%d\n",
           (unsigned long)getuid(), secure, value);
    if (argc != 2 || value != 42) {
        return 1;
    }
    return secure != strtoul(argv[1], NULL, 10) || (secure && getuid() == 0);
}
C
stager="$repo/packaging/linux/stage-install.sh"
explicit=(--host "$work/hermes" --assets "$work/assets" --version-header "$work/hermes-build-version.h")
stage() {
  "$stager" --mcpp "$1" "${explicit[@]}" --pyrowave-prefix "$sdk" --libdir "$2" > /dev/null
}

check_layout() {
  local libdir=$1
  local expected="/usr/$libdir/hermes"
  local payload="$work/payload-${libdir//\//-}"
  cc "$work/host.c" -L"$sdk/lib" -lpyrowave-shared \
    -Wl,--disable-new-dtags,-rpath,"$expected" -o "$work/hermes"
  stage "$payload" "$libdir"

  local original staged
  original=$(readelf -d "$work/hermes")
  staged=$(readelf -d "$payload/usr/bin/hermes")
  grep -F '(RPATH)' <<< "$original" | grep -F "[$expected]"
  grep -F '(RUNPATH)' <<< "$staged" | grep -F "[$expected]"
  grep -F 'Shared library: [libpyrowave-shared.so.0]' <<< "$staged"
  [[ $(readlink "$payload$expected/libpyrowave-shared.so.0") == libpyrowave-shared.so.0.5.0 ]]
  [[ $(readlink "$payload$expected/libpyrowave-shared.so") == libpyrowave-shared.so.0 ]]
  cmp "$sdk/share/licenses/pyrowave/LICENSE" "$payload/usr/share/licenses/hermes/pyrowave/LICENSE"
  grep -Fq '<release version="2026.701.0" date="2026-07-01">' "$payload/usr/share/metainfo/io.github.mroz59.Hermes.metainfo.xml"
}
for libdir in lib lib64 lib/x86_64-linux-gnu; do
  check_layout "$libdir"
done

reject_stage() {
  local diagnostic=$1
  shift
  if "$stager" --mcpp "$work/rejected" "$@" > "$work/rejected.log" 2>&1; then
    printf 'Expected staging rejection: %s\n' "$diagnostic" >&2
    exit 1
  fi
  grep -F -- "$diagnostic" "$work/rejected.log"
  [[ ! -e $work/rejected ]]
  [[ -z $(find "$work" -maxdepth 1 -name 'hermes-stage.*' -print -quit) ]]
}

result="$work/result with spaces.json"
pointer="$work/last-result.json"
tools="$work/tools-result.json"
jq -cn --arg path "$work/hermes" '{kind:"mcpp.result",data:{status:"ok",artifacts:[{kind:"bin",name:"hermes",path:$path}]}}' > "$result"
jq -cn --arg result "$result" '{kind:"mcpp.last-result",data:{result:$result}}' > "$pointer"
jq -cn --arg out "$work" '{graph:{packages:[{package:{name:"hermes-host"},outDir:$out}]}}' > "$work/resolution.json"
jq -cn --arg path "$work/hermes" '{kind:"mcpp.result",data:{status:"ok",artifacts:[{kind:"bin",name:"hermes-gamemode",path:$path}]}}' > "$tools"
"$stager" --mcpp "$work/result payload" --result "$pointer" --result "$tools" --libdir lib/x86_64-linux-gnu > "$work/result.list"
grep -Fxq 'usr/bin/hermes-gamemode' "$work/result.list"
cmp "$work/result payload/usr/bin/hermes" "$work/payload-lib-x86_64-linux-gnu/usr/bin/hermes"

reject_stage 'Hermes must have install RUNPATH /usr/lib64/hermes' "${explicit[@]}" --libdir lib64
reject_stage 'PyroWave stage prefix differs from the host-linked SDK' "${explicit[@]}" \
  --libdir lib/x86_64-linux-gnu --pyrowave-prefix "$work/other sdk"
reject_stage '--result cannot be combined with explicit artifact paths' --result "$result" "${explicit[@]}"
jq '.data.artifacts[0].path = 4' "$result" > "$work/bad-result.json"
reject_stage 'cannot read mcpp artifacts' --result "$work/bad-result.json"
jq '.data.artifacts[0].path = "/another/hermes"' "$result" > "$work/conflict-result.json"
reject_stage 'conflicting mcpp results for hermes' --result "$result" --result "$work/conflict-result.json"

runtime="$sdk/lib/libpyrowave-shared.so.0.5.0"
cp -- "$runtime" "$work/original-runtime"
printf 'modified library\n' >> "$runtime"
reject_stage 'PyroWave runtime differs from the library used to link Hermes' "${explicit[@]}" --libdir lib/x86_64-linux-gnu
mv -- "$work/original-runtime" "$runtime"

if $layout_only; then
  echo 'Native mcpp staging layout passed. AT_SECURE was skipped.'
  exit 0
fi

# Only the CI container's secure runtime check needs a real installed absolute
# path. A unique directory keeps the fixture independent of installed packages.
secure_libdir=$(mktemp -d /usr/lib/hermes-mcpp-loader.XXXXXX)
chmod 755 "$secure_libdir"
libdir=${secure_libdir#/usr/}
check_layout "$libdir"
payload="$work/payload-${libdir//\//-}"
cp -a "$payload$secure_libdir/hermes" "$secure_libdir/hermes"
runuser -u nobody -- env -i PATH=/usr/bin:/bin "$payload/usr/bin/hermes" 0
setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p "$payload/usr/bin/hermes"
runuser -u nobody -- env -i PATH=/usr/bin:/bin "$payload/usr/bin/hermes" 1
echo 'Native mcpp staging and secure loader passed.'
