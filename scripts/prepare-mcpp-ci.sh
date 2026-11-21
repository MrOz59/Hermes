#!/usr/bin/env bash
# Prepare the pinned mcpp release and link existing Linux runner tools.
set -euo pipefail

: "${MCPP_HOME:?set MCPP_HOME to a runner-local directory}"
: "${MCPP_TOOLCHAIN:?set MCPP_TOOLCHAIN to path:<runner-local directory>}"
if [[ $MCPP_TOOLCHAIN != path:/* || $(uname -m) != x86_64 ]]; then
    echo "Expected a path: toolchain and a Linux x86-64 runner" >&2
    exit 1
fi

version=2026.11.20.1
archive="mcpp-$version-linux-x86_64.tar.gz"
if [[ ! -x $MCPP_HOME/bin/mcpp ]]; then
    mkdir -p "$MCPP_HOME"
    download=$(mktemp "$MCPP_HOME/$archive.XXXXXX")
    trap 'rm -f "$download"' EXIT
    curl -fLsS --retry 3 "https://github.com/mcpp-community/mcpp/releases/download/v$version/$archive" -o "$download"
    printf '%s  %s\n' 410053964917d54ca1baf74909a13089629a1aef5d8eefee994debcdd63bc840 "$download" | sha256sum -c -
    tar -xzf "$download" -C "$MCPP_HOME" --strip-components=1
fi
[[ $("$MCPP_HOME/bin/mcpp" --version) == "mcpp $version" ]]
test -x "$MCPP_HOME/registry/bin/xlings"

toolchain=${MCPP_TOOLCHAIN#path:}
suffix=${1:+-$1}
mkdir -p "$toolchain/bin"
for driver in gcc g++; do
    ln -sfnT "$(command -v "$driver$suffix")" "$toolchain/bin/$driver-system"
done
for tool in ar as ld nm objcopy objdump ranlib readelf strip; do
    ln -sfnT "$(command -v "$tool")" "$toolchain/bin/$tool"
done

adapter=$(mktemp "$toolchain/bin/compiler-adapter.XXXXXX")
trap 'rm -f "${download:-}" "$adapter"' EXIT
cat > "$adapter" <<'ADAPTER'
#!/usr/bin/env bash
set -euo pipefail

name=${0##*/}
driver="${0%/*}/$name-system"
case "$#:${1:-}" in
    1:--version)
        printf '%s (GCC) %s\n' "$name" "$("$driver" -dumpfullversion)"
        exit
        ;;
    1:-dumpmachine)
        machine=$("$driver" -dumpmachine)
        case "$machine" in
            x86_64-redhat-linux) machine=x86_64-linux-gnu ;;
        esac
        printf '%s\n' "$machine"
        exit
        ;;
esac

compiler_version=$("$driver" -dumpfullversion)
if [[ $compiler_version != 14.* ]]; then
    exec "$driver" "$@"
fi

# GCC 14 expects its original spelling and preprocessing mode for scans.
scan=0
for option; do
    case "$option" in -fdeps-format=*) scan=1 ;; esac
done
args=()
for option; do
    case "$scan:$option" in
        *:-fmodules) args+=(-fmodules-ts) ;;
        1:-M) args+=(-MMD) ;;
        1:-MM) ;;
        *) args+=("$option") ;;
    esac
done
exec "$driver" "${args[@]}"
ADAPTER
chmod 755 "$adapter"
mv -Tf "$adapter" "$toolchain/bin/compiler-adapter"
for driver in gcc g++; do
    ln -sfnT compiler-adapter "$toolchain/bin/$driver"
done

# Match the versioned bootstrap paths read by the pinned mcpp release.
registry="$MCPP_HOME/registry"
ninja_dir="$registry/data/xpkgs/xim-x-ninja/$(ninja --version)"
patchelf_dir="$registry/data/xpkgs/xim-x-patchelf/0.18.0/bin"
patchelf_bin="$(command -v patchelf || true)"
if [[ -z $patchelf_bin || $("$patchelf_bin" --version) != "patchelf 0.18.0" ]]; then
    patchelf_prefix="$MCPP_HOME/bootstrap/patchelf-0.18.0"
    patchelf_bin="$patchelf_prefix/bin/patchelf"
    if [[ ! -x $patchelf_bin ]]; then
        mkdir -p "$patchelf_prefix"
        (
            archive_tmp="$(mktemp "$patchelf_prefix/patchelf.XXXXXX")"
            trap 'rm -f -- "$archive_tmp"' EXIT
            curl -fLsS --retry 3 \
                https://github.com/NixOS/patchelf/releases/download/0.18.0/patchelf-0.18.0-x86_64.tar.gz \
                -o "$archive_tmp"
            printf '%s  %s\n' ce84f2447fb7a8679e58bc54a20dc2b01b37b5802e12c57eece772a6f14bf3f0 "$archive_tmp" | sha256sum -c -
            tar -xzf "$archive_tmp" -C "$patchelf_prefix"
        )
    fi
    [[ $("$patchelf_bin" --version) == "patchelf 0.18.0" ]]
fi
subos="$registry/subos/default"
mkdir -p "$subos/bin" "$ninja_dir" "$patchelf_dir"
ln -sfnT default "$registry/subos/current"
ln -sfnT ../../../bin/xlings "$subos/bin/xlings"
ln -sfnT "$(command -v ninja)" "$ninja_dir/ninja"
ln -sfnT "$patchelf_bin" "$patchelf_dir/patchelf"
printf '{"subos_info":{"runtime":"host","runtime_source":"host","host_glibc":"%s","runtime_abi":"linux-x86_64-glibc","schema_version":1},"workspace":{}}\n' \
    "$(getconf GNU_LIBC_VERSION | cut -d ' ' -f2)" > "$subos/.xlings.json"
cat > "$MCPP_HOME/config.toml" <<'CONFIG'
[target.x86_64-linux-gnu]
sysroot = "glibc@system"
CONFIG
