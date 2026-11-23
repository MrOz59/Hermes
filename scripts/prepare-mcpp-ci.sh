#!/usr/bin/env bash
# Prepare the pinned mcpp release and link existing Linux runner tools.
set -euo pipefail

: "${MCPP_HOME:?set MCPP_HOME to a runner-local directory}"
: "${MCPP_TOOLCHAIN:?set MCPP_TOOLCHAIN to path:<runner-local directory>}"
if [[ $MCPP_TOOLCHAIN != path:/* || $(uname -m) != x86_64 ]]; then
    echo "Expected a path: toolchain and a Linux x86-64 runner" >&2
    exit 1
fi

version=2026.11.21.1
archive="mcpp-$version-linux-x86_64.tar.gz"
if [[ ! -x $MCPP_HOME/bin/mcpp ]]; then
    mkdir -p "$MCPP_HOME"
    (
        download=$(mktemp "$MCPP_HOME/$archive.XXXXXX")
        trap 'rm -f -- "$download"' EXIT
        curl -fLsS --retry 3 "https://github.com/mcpp-community/mcpp/releases/download/v$version/$archive" -o "$download"
        printf '%s  %s\n' bafefffb6651b47e3e1be0c85f12df091a9e60d8f635537c6812295dc2327ac2 "$download" | sha256sum -c -
        tar -xzf "$download" -C "$MCPP_HOME" --strip-components=1
    )
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

# mcpp reads the driver version and vendor triple directly. GCC 14 still
# needs its earlier module scanner flags.
gcc_version=$("$(readlink -- "$toolchain/bin/gcc-system")" -dumpfullversion)
if [[ $gcc_version == 14.* ]]; then
    adapter=$(mktemp "$toolchain/bin/compiler-adapter.XXXXXX")
    trap 'rm -f -- "$adapter"' EXIT
    cat > "$adapter" <<'ADAPTER'
#!/usr/bin/env bash
set -euo pipefail

name=${0##*/}
driver=$(readlink -- "${0%/*}/$name-system")
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
else
    for driver in gcc g++; do
        ln -sfnT "$driver-system" "$toolchain/bin/$driver"
    done
fi

# Initialize the local provider's host-glibc environment without fetching a
# managed toolchain. mcpp takes Ninja from PATH and does not need patchelf
# to build. Native package staging supplies patchelf on its own runners.
registry="$MCPP_HOME/registry"
subos="$registry/subos/default"
mkdir -p "$subos/bin"
ln -sfnT default "$registry/subos/current"
ln -sfnT ../../../bin/xlings "$subos/bin/xlings"
printf '{"subos_info":{"runtime":"host","runtime_source":"host","host_glibc":"%s","runtime_abi":"linux-x86_64-glibc","schema_version":1},"workspace":{}}\n' \
    "$(getconf GNU_LIBC_VERSION | cut -d ' ' -f2)" > "$subos/.xlings.json"
cat > "$MCPP_HOME/config.toml" <<'CONFIG'
[build]
allow_host_tools = ["ninja"]
offline = true

[target.x86_64-linux-gnu]
sysroot = "glibc@system"
CONFIG
