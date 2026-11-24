#!/usr/bin/env bash
# Install the private PyroWave SDK from an already successful mcpp build.
set -euo pipefail
if (( $# != 1 )); then
    echo "usage: $0 SDK_DIRECTORY" >&2
    exit 2
fi
sdk=$1
result=$(jq -er '.data.result | strings | select(length > 0)' "$sdk/target/.last-result.json")
library=$(jq -er 'first(.data.artifacts[] | select(.name == "pyrowave-shared").path | strings)' "$result") || {
    echo "mcpp did not produce the PyroWave shared library" >&2
    exit 1
}
package=$(jq -cer 'first(.graph.packages[] | select(.package.name == "pyrowave-sdk"))' "$(dirname -- "$result")/resolution.json") || {
    echo "mcpp did not report the PyroWave SDK package" >&2
    exit 1
}
version=$(jq -er '.package.version | strings | select(test("^[0-9]+\\.[0-9]+\\.[0-9]+([+-][A-Za-z0-9.-]+)?$"))' <<< "$package")
output=$(jq -er '.outDir | strings | select(length > 0)' <<< "$package")
validator="$output/pyrowave-device-validation"

prefix="$sdk/install"
libdir="$prefix/lib"
includedir="$prefix/include/pyrowave"
pkgconfig="$prefix/share/pkgconfig"
bindir="$prefix/bin"
mkdir -p -- "$libdir" "$includedir" "$pkgconfig" "$bindir"

filename="libpyrowave-shared.so.$version"
soname="libpyrowave-shared.so.${version%%.*}"
install -m 0644 -- "$library" "$libdir/$filename"
ln -sfn -- "$filename" "$libdir/$soname"
ln -sfn -- "$soname" "$libdir/libpyrowave-shared.so"
cp -- "$sdk/src/pyrowave.h" "$includedir/pyrowave.h"
cp -p -- "$validator" "$bindir/pyrowave-device-validation"

# Quote replacement values so paths containing ampersands remain literal.
shopt -u patsub_replacement
metadata=$(<"$sdk/src/pkg-config/pyrowave-shared.pc.in")
metadata=${metadata//@CMAKE_INSTALL_PREFIX@/$prefix}
metadata=${metadata//@PYROWAVE_INSTALL_LIB_DIR@/$libdir}
metadata=${metadata//@PYROWAVE_INSTALL_INC_DIR@/$includedir}
metadata=${metadata//@PYROWAVE_API_VERSION@/$version}
printf '%s\n' "$metadata" > "$pkgconfig/pyrowave-shared.pc"
