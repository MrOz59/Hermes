#!/usr/bin/env bash

# Build the Fedora RPM from CMake's complete install tree.  Keeping this in a
# script, rather than a workflow heredoc, makes the package layout testable and
# prevents release jobs from silently omitting systemd/udev integration.

set -euo pipefail

build_dir="${1:-build}"
version="${2:-${BUILD_VERSION:-}}"
output_dir="${3:-.}"

if [[ -z "${version}" ]]; then
  printf 'usage: %s BUILD_DIR VERSION [OUTPUT_DIR]\n' "$0" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="$(realpath "${build_dir}")"
mkdir -p "${output_dir}"
output_dir="$(realpath "${output_dir}")"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/hermes-rpm.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT

stage="${work_dir}/hermes-root-${version}"
mkdir -p "${stage}"
DESTDIR="${stage}" cmake --install "${build_dir}" --strip
install -D -m 0644 "${repo_root}/LICENSE" \
  "${stage}/usr/share/licenses/hermes/LICENSE"

# The historical CMake target is still named sunshine and, because it carries a
# VERSION property, installs a versioned file plus a symlink.  Packages expose
# only the Hermes product name.
installed_binary="$(find "${stage}/usr/bin" -maxdepth 1 -type f \
  \( -name 'sunshine-*' -o -name sunshine \) -print -quit)"
if [[ -z "${installed_binary}" ]]; then
  printf 'CMake install did not produce a sunshine executable\n' >&2
  exit 1
fi
rm -f "${stage}/usr/bin/sunshine"
mv "${installed_binary}" "${stage}/usr/bin/hermes"

if [[ -f "${stage}/usr/lib/systemd/user/sunshine.service" ]]; then
  mv "${stage}/usr/lib/systemd/user/sunshine.service" \
    "${stage}/usr/lib/systemd/user/hermes.service"
fi

# These are the minimum integration files promised by the installation docs.
required_files=(
  usr/bin/hermes
  usr/bin/hermes-gamescope-launch
  usr/bin/hermes-kms-card-broker
  usr/bin/hermes-monitor-recovery
  usr/bin/hermes-session-broker
  usr/lib/modules-load.d/60-hermes.conf
  usr/lib/systemd/system/hermes-kms-card-broker.service
  usr/lib/systemd/system/hermes-kms-card-broker.socket
  usr/lib/systemd/system/hermes-session-broker.service
  usr/lib/systemd/system/hermes-session-broker.socket
  usr/lib/systemd/user/hermes.service
  usr/lib/udev/rules.d/60-hermes.rules
  usr/share/applications/io.github.mroz59.Hermes.desktop
  usr/share/hermes/web/index.html
  usr/share/icons/hicolor/scalable/apps/hermes.svg
  usr/share/metainfo/io.github.mroz59.Hermes.metainfo.xml
  usr/share/polkit-1/rules.d/10-hermes-session-deny.rules
)
for required in "${required_files[@]}"; do
  if [[ ! -e "${stage}/${required}" ]]; then
    printf 'RPM staging tree is missing %s\n' "${required}" >&2
    exit 1
  fi
done

topdir="${work_dir}/rpmbuild"
mkdir -p "${topdir}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
tar -C "${work_dir}" -czf \
  "${topdir}/SOURCES/hermes-root-${version}.tar.gz" \
  "hermes-root-${version}"

cat > "${topdir}/SPECS/hermes.spec" <<'EOF'
Name:           hermes
Version:        __HERMES_VERSION__
Release:        1%{?dist}
Summary:        Self-hosted game stream host with Linux virtual display support
License:        GPL-3.0-only
URL:            https://github.com/MrOz59/Hermes
Source0:        hermes-root-%{version}.tar.gz

Requires:       libcap
Requires:       systemd-udev
%global debug_package %{nil}

%description
Hermes is an Apollo-derived game streaming server with low-latency Linux
virtual displays via Hermes-KMS. EVDI remains available as a fallback.

%prep
%setup -q -n hermes-root-%{version}

%build

%install
mkdir -p %{buildroot}
cp -a usr %{buildroot}/

%files
%license /usr/share/licenses/hermes/LICENSE
%caps(cap_sys_admin+p) /usr/bin/hermes
/usr/bin/hermes-*
/usr/lib/modules-load.d/60-hermes.conf
/usr/lib/systemd/system/hermes-*.service
/usr/lib/systemd/system/hermes-*.socket
/usr/lib/systemd/user/hermes.service
/usr/lib/sysusers.d/hermes.conf
/usr/lib/udev/rules.d/60-hermes.rules
/usr/share/applications/io.github.mroz59.Hermes*.desktop
/usr/share/hermes
/usr/share/icons/hicolor/scalable/apps/hermes.svg
/usr/share/icons/hicolor/scalable/status/hermes*.svg
/usr/share/metainfo/io.github.mroz59.Hermes.metainfo.xml
/usr/share/polkit-1/rules.d/10-hermes-session-deny.rules
EOF

sed -i "s/__HERMES_VERSION__/${version}/" "${topdir}/SPECS/hermes.spec"
rpmbuild -bb --define "_topdir ${topdir}" "${topdir}/SPECS/hermes.spec"

find "${topdir}/RPMS" -type f -name '*.rpm' -exec cp -v {} "${output_dir}/" \;
