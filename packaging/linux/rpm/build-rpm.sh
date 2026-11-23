#!/usr/bin/env bash
# Turn a staged native Hermes install tree into one Fedora RPM.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
payload="$(realpath -e -- "${1:-$repo/target/hermes-payload}")"
release=${2:-1}
if [[ $# -gt 2 || ! $release =~ ^[0-9]+$ || ! -x "$payload/usr/bin/hermes" ]]; then
    printf 'usage: %s [staged-payload [rpm-release-number]]\n' "$0" >&2
    exit 2
fi
version="$(python3 - "$payload" <<'PY'
from pathlib import Path
import re
import sys
from xml.etree import ElementTree

metainfo = Path(sys.argv[1]) / "usr/share/metainfo/io.github.mroz59.Hermes.metainfo.xml"
release = ElementTree.parse(metainfo).find("releases/release")
version = release.get("version", "") if release is not None else ""
if not re.fullmatch(r"[A-Za-z0-9._+~^]+", version):
    sys.exit(f"Invalid or missing RPM version in {metainfo}")
print(version)
PY
)"

mkdir -p "$repo/target"
work="$(mktemp -d "$repo/target/rpmbuild.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS,tmp}
tar --transform="flags=r;s|^[^/]*|hermes-$version|" \
    -czf "$work/SOURCES/hermes-$version.tar.gz" -C "$(dirname "$payload")" "$(basename "$payload")"
(
    cd "$payload"
    {
        find . -type d -path './usr/share/hermes*' -printf '%%dir /%P\n'
        find . \( -type f -o -type l \) -printf '/%P\n'
    } | sort
) > "$work/SOURCES/hermes-$version.files"

macros=(
    --define "_topdir $work"
    --define "_rpmdir $work/RPMS"
    --define "_tmppath $work/tmp"
    --define "hermes_version $version"
    --define "hermes_release $release"
)
if [[ -f "$payload$(rpm --eval '%{_libdir}')/hermes/libpyrowave-shared.so.0" ]]; then
    macros+=(--define 'hermes_bundled_pyrowave 1')
fi
TMPDIR="$work/tmp" rpmbuild "${macros[@]}" -bb "$repo/packaging/linux/rpm/hermes.spec" >&2

mapfile -d '' packages < <(find "$work/RPMS" -type f -name '*.rpm' -print0)
if [[ ${#packages[@]} -ne 1 ]]; then
    printf 'Expected one binary Hermes RPM, found %s\n' "${#packages[@]}" >&2
    exit 1
fi
package=${packages[0]}
destination="$repo/target/dist/rpm/${package#"$work/RPMS/"}"
mkdir -p "$(dirname "$destination")"
mv -nT -- "$package" "$destination"
if [[ -e "$package" ]]; then
    printf 'RPM already exists at %s. Choose a different RPM release number.\n' "$destination" >&2
    exit 1
fi
printf '%s\n' "$destination"
