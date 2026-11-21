#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$(realpath -m -- "${1:-${repo_root}/build/pyrowave-sdk}")"
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
cp "$repo_root/mcpp/pyrowave-sdk/"{mcpp.toml,build.mcpp} "$sdk/"
(
    cd "$sdk"
    mcpp_bin="${MCPP_HOME:-}/bin/mcpp"
    if [[ -z ${MCPP_HOME:-} || ! -x $mcpp_bin ]]; then
        mcpp_bin="$(command -v mcpp)"
    fi
    "$mcpp_bin" build --offline --release -j "${MCPP_JOBS:-2}"
)

python3 - "$sdk" <<'PYTHON'
import json
from pathlib import Path
import shutil
import sys
import tomllib

sdk = Path(sys.argv[1])
version = tomllib.loads((sdk / "mcpp.toml").read_text())["package"]["version"]
last = json.loads((sdk / "target/.last-result.json").read_text())["data"]
result_path = Path(last["result"])
result = json.loads(result_path.read_text())["data"]
library = next(Path(item["path"]) for item in result["artifacts"] if item["name"] == "pyrowave-shared")
resolution = json.loads(result_path.with_name("resolution.json").read_text())
package = next(entry for entry in resolution["graph"]["packages"] if entry["package"]["name"] == "pyrowave-sdk")
validator = Path(package["outDir"]) / "pyrowave-device-validation"

prefix = sdk / "install"
libdir = prefix / "lib"
includedir = prefix / "include/pyrowave"
pkgconfig = prefix / "share/pkgconfig"
bindir = prefix / "bin"
for directory in (libdir, includedir, pkgconfig, bindir):
    directory.mkdir(parents=True, exist_ok=True)

filename = f"libpyrowave-shared.so.{version}"
soname = f"libpyrowave-shared.so.{version.split('.')[0]}"
shutil.copyfile(library, libdir / filename)
(libdir / filename).chmod(0o644)
for alias, destination in ((soname, filename), ("libpyrowave-shared.so", soname)):
    link = libdir / alias
    link.unlink(missing_ok=True)
    link.symlink_to(destination)
shutil.copyfile(sdk / "src/pyrowave.h", includedir / "pyrowave.h")
shutil.copy2(validator, bindir / validator.name)

metadata = (sdk / "src/pkg-config/pyrowave-shared.pc.in").read_text()
for key, value in (
    ("CMAKE_INSTALL_PREFIX", prefix),
    ("PYROWAVE_INSTALL_LIB_DIR", libdir),
    ("PYROWAVE_INSTALL_INC_DIR", includedir),
    ("PYROWAVE_API_VERSION", version),
):
    metadata = metadata.replace(f"@{key}@", str(value))
(pkgconfig / "pyrowave-shared.pc").write_text(metadata)
PYTHON

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
