#!/usr/bin/env bash
# Run inside a disposable package CI container, after installing the real package.
set -euo pipefail
libdir=${1:?usage: package-loader-smoke.sh /absolute/private/library/directory}
binary=/usr/bin/hermes
[[ $EUID == 0 && $libdir == /* ]]
test -f "$libdir/libpyrowave-shared.so.0"
# A capability-bearing host must never load a foreign-owned or writable codec.
unsafe=$(find "$libdir" -type f \( ! -user root -o -perm /022 \) -print)
[[ -z $unsafe ]]
readelf -d "$binary" | grep -F 'Shared library: [libpyrowave-shared.so.0]'
runpath=$(readelf -d "$binary" | sed -n 's/.*(RUNPATH).*\[\(.*\)\].*/\1/p')
[[ $runpath == "$libdir" ]]
caps=$(getcap "$binary")
for cap in cap_sys_admin cap_mknod cap_sys_chroot; do
  [[ $caps == *"$cap"* ]]
done
[[ $caps == *'=p' ]]

# A real exec is essential: invoking ld.so/ldd directly bypasses file caps.
# Link the probe to the installed codec and use the very same capabilities.
work=$(mktemp -d /tmp/hermes-package-loader.XXXXXX)
trap 'rm -rf "$work"' EXIT
chmod 755 "$work"
cat > "$work/probe.c" <<'C'
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>
int main(void) {
    unsigned long secure = getauxval(AT_SECURE);
    printf("uid=%lu AT_SECURE=%lu\n", (unsigned long)getuid(), secure);
    return getuid() == 0 || secure != 1;
}
C
cc "$work/probe.c" -Wl,--no-as-needed -L"$libdir" -l:libpyrowave-shared.so.0 \
  -Wl,-rpath,"$libdir" -o "$work/probe"
setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p "$work/probe"
useradd --create-home --shell /bin/sh hermes-loader-test
runuser -u hermes-loader-test -- env -i PATH=/usr/bin:/bin "$work/probe"
runuser -u hermes-loader-test -- env -i PATH=/usr/bin:/bin \
  HOME=/home/hermes-loader-test timeout 20 "$binary" --help > "$work/help.txt" 2>&1
cat "$work/help.txt"
grep -qi 'usage' "$work/help.txt"
echo 'Installed Hermes starts with capabilities as a non-root user.'
