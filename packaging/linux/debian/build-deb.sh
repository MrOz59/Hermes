#!/usr/bin/env bash

# Build a Debian package from the same validated CMake install tree used by the
# RPM job. Runtime library dependencies remain release-specific and are derived
# inside the target Ubuntu container.

set -euo pipefail

build_dir="${1:-build}"
version="${2:-${BUILD_VERSION:-}}"
distribution="${3:-ubuntu}"
output_dir="${4:-.}"

if [[ -z "$version" ]]; then
  printf 'usage: %s BUILD_DIR VERSION DISTRIBUTION [OUTPUT_DIR]\n' "$0" >&2
  exit 2
fi
if [[ ! "$version" =~ ^[A-Za-z0-9.+~]+$ ]] || \
   [[ ! "$distribution" =~ ^[A-Za-z0-9.+~]+$ ]]; then
  printf 'build-deb: unsafe version or distribution label\n' >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="$(realpath "$build_dir")"
mkdir -p "$output_dir"
output_dir="$(realpath "$output_dir")"

for command_name in cmake dpkg dpkg-deb dpkg-shlibdeps; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'build-deb: required command not found: %s\n' "$command_name" >&2
    exit 1
  fi
done

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/hermes-deb.XXXXXX")"
trap 'rm -rf -- "$work_dir"' EXIT

architecture="$(dpkg --print-architecture)"
stage="$work_dir/hermes_${version}_${architecture}"
"$repo_root/packaging/linux/stage-install.sh" "$build_dir" "$stage" >/dev/null

shlibdeps_dir="$work_dir/shlibdeps"
mkdir -p "$shlibdeps_dir/debian"
printf 'Source: hermes\nPackage: hermes\nArchitecture: %s\n' "$architecture" \
  > "$shlibdeps_dir/debian/control"
deb_depends="$(
  cd "$shlibdeps_dir"
  dpkg-shlibdeps -O --ignore-missing-info "$stage/usr/bin/hermes" \
    | sed 's/^shlibs:Depends=//'
)"
if [[ -z "$deb_depends" ]]; then
  printf 'build-deb: dpkg-shlibdeps produced no dependencies\n' >&2
  exit 1
fi

mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<EOF
Package: hermes
Version: $version
Section: net
Priority: optional
Architecture: $architecture
Depends: libcap2-bin, $deb_depends
Recommends: evdi-dkms
Maintainer: Hermes contributors
Description: Self-hosted game stream host with Linux virtual display support
 Hermes is an Apollo-derived game streaming server with low-latency Linux
 virtual displays via Hermes-KMS. EVDI remains available as a fallback.
EOF

cat > "$stage/DEBIAN/postinst" <<'EOF'
#!/bin/bash
set -e

setcap cap_sys_admin+p /usr/bin/hermes || true
if command -v modprobe >/dev/null 2>&1; then
  modprobe evdi 2>/dev/null || true
fi
gtk-update-icon-cache -f /usr/share/icons/hicolor/ 2>/dev/null || true
update-desktop-database /usr/share/applications 2>/dev/null || true
EOF
chmod 0755 "$stage/DEBIAN/postinst"

package="$output_dir/hermes_${version}_${distribution}_${architecture}.deb"
dpkg-deb --root-owner-group --build "$stage" "$package"
dpkg-deb --info "$package" >/dev/null

package_contents="$(dpkg-deb --contents "$package" | awk '{print $NF}')"
for required in \
  ./usr/bin/hermes \
  ./usr/lib/systemd/user/hermes.service \
  ./usr/share/applications/io.github.mroz59.Hermes.desktop
do
  grep -Fxq "$required" <<< "$package_contents" || {
    printf 'build-deb: package is missing %s\n' "$required" >&2
    exit 1
  }
done

printf '%s\n' "$package"
