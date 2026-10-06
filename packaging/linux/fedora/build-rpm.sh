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
if [[ ! "${version}" =~ ^[A-Za-z0-9.+~]+$ ]]; then
  printf 'build-rpm: unsafe version: %s\n' "${version}" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="$(realpath "${build_dir}")"
mkdir -p "${output_dir}"
output_dir="$(realpath "${output_dir}")"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/hermes-rpm.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT

stage="${work_dir}/hermes-root-${version}"
"${repo_root}/packaging/linux/stage-install.sh" \
  "${build_dir}" "${stage}" >/dev/null

topdir="${work_dir}/rpmbuild"
mkdir -p "${topdir}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
tar -C "${work_dir}" -czf \
  "${topdir}/SOURCES/hermes-root-${version}.tar.gz" \
  "hermes-root-${version}"

# Keep package contents synchronized with CMake. The two specially marked files
# are excluded here and declared explicitly below with their RPM attributes.
file_list="${work_dir}/file-list"
(
  cd "${stage}"
  find . -type d -path './usr/share/hermes*' -printf '%%dir /%P\n'
  find . \( -type f -o -type l \) \
    ! -path './usr/bin/hermes' \
    ! -path './usr/share/licenses/hermes/LICENSE' \
    -printf '/%P\n'
) | sort > "${file_list}"

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
%defattr(-,root,root,-)
%license /usr/share/licenses/hermes/LICENSE
%caps(cap_sys_admin+p) /usr/bin/hermes
EOF

cat "${file_list}" >> "${topdir}/SPECS/hermes.spec"

sed -i "s/__HERMES_VERSION__/${version}/" "${topdir}/SPECS/hermes.spec"
rpmbuild -bb --define "_topdir ${topdir}" "${topdir}/SPECS/hermes.spec"

mapfile -t packages < <(find "${topdir}/RPMS" -type f -name '*.rpm')
if [[ ${#packages[@]} -ne 1 ]]; then
  printf 'build-rpm: expected one RPM, found %d\n' "${#packages[@]}" >&2
  exit 1
fi
package_metadata="$(
  rpm -qp --queryformat '[%{FILENAMES} %{FILECAPS}\n]' "${packages[0]}"
)"
grep -Fq '/usr/bin/hermes cap_sys_admin=p' <<< "${package_metadata}"
package_contents="$(rpm -qlp "${packages[0]}")"
grep -Fxq /usr/lib/systemd/user/hermes.service <<< "${package_contents}"
grep -Fxq /usr/share/applications/io.github.mroz59.Hermes.desktop \
  <<< "${package_contents}"
cp -v "${packages[0]}" "${output_dir}/"
