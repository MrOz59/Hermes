#!/usr/bin/env bash
# Runs the installer with --dry-run --yes against the real GitHub releases, as
# if on another distribution, and checks which packages it picks. Nothing is
# installed: a dry run only prints commands. Needs root for a private mount
# namespace (the distribution is faked by bind-mounting an os-release file)
# and a non-root user to run the installer as.
#
#   sudo scripts/tests/installer-dry-run.sh USER
#
# Export GITHUB_TOKEN to avoid the anonymous API limit.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "run as root (it needs a mount namespace)" >&2; exit 2; }
user=${1:?usage: installer-dry-run.sh USER}
script="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/install.sh"
fixtures=$(mktemp -d -t hermes-dry-run.XXXXXX)
chmod 755 "$fixtures"
trap 'rm -rf -- "$fixtures"' EXIT

os_release() {
  case "$1" in
    arch) printf 'NAME="Arch Linux"\nID=arch\nPRETTY_NAME="Arch Linux"\n' ;;
    cachyos) printf 'NAME="CachyOS Linux"\nID=cachyos\nID_LIKE=arch\nPRETTY_NAME="CachyOS"\n' ;;
    ubuntu-*) printf 'NAME="Ubuntu"\nID=ubuntu\nVERSION_ID=%s\nPRETTY_NAME="Ubuntu %s"\n' "${1#ubuntu-}" "${1#ubuntu-}" ;;
    fedora-*) printf 'NAME="Fedora Linux"\nID=fedora\nVERSION_ID=%s\nPRETTY_NAME="Fedora Linux %s"\n' "${1#fedora-}" "${1#fedora-}" ;;
  esac
}

failures=0
# check DISTRO "ARGS" PATTERN... : each pattern must appear in the output, a
# pattern starting with ! must not.
check() {
  local distro=$1 args=$2 pattern log rc=0
  shift 2
  os_release "$distro" >"$fixtures/os-release-$distro"
  log="$fixtures/$distro${args// /}.log"
  # shellcheck disable=SC2086 # args are word-split on purpose
  unshare -m --propagation private bash -c '
    mount --bind "$1" /etc/os-release
    shift
    exec sudo --preserve-env=GITHUB_TOKEN,GH_TOKEN -u "$@"
  ' _ "$fixtures/os-release-$distro" "$user" bash "$script" --dry-run --yes $args >"$log" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL: $distro $args exited $rc" >&2
    sed 's/^/    /' "$log" >&2
    failures=$((failures + 1))
    return 0
  fi
  for pattern in "$@"; do
    if [[ "$pattern" == !* ]]; then
      if grep -Eq -- "${pattern#!}" "$log"; then
        echo "FAIL: $distro $args printed ${pattern#!}" >&2
        failures=$((failures + 1))
      fi
    elif ! grep -Eq -- "$pattern" "$log"; then
      echo "FAIL: $distro $args did not print $pattern" >&2
      sed 's/^/    /' "$log" >&2
      failures=$((failures + 1))
    fi
  done
  echo "ok: $distro $args"
}

kms_core='hermes-kms-dkms'
check arch "" 'pacman -U --noconfirm .*/hermes-streaming-[0-9]' "pacman -U --noconfirm .*/${kms_core}-git-0[.]4[.]1" '!hermes-streaming-debug'
check cachyos "--nightly" 'pacman -U --noconfirm .*/hermes-streaming-[0-9][^ ]*[+]' '!hermes-streaming-debug' '!pacman -U --noconfirm [^ ]*%2B'
check ubuntu-24.04 "" 'apt-get install -y .*/hermes_[0-9][^ ]*_ubuntu24[.]04_amd64[.]deb' "apt-get install -y .*/${kms_core}_0[.]4[.]1"
check ubuntu-26.04 "--nightly" 'apt-get install -y .*/hermes_[0-9][^ ]*_ubuntu26[.]04_amd64[.]deb'
check fedora-43 "" 'dnf install -y .*/hermes-[0-9][^ ]*[.]fc43[.]x86_64[.]rpm' "dnf install -y .*/${kms_core}-0[.]4[.]1-[^ ]*[.]noarch[.]rpm"
check fedora-44 "" 'dnf install -y .*/hermes-[0-9][^ ]*[.]fc44[.]x86_64[.]rpm' "dnf install -y .*/${kms_core}-0[.]4[.]1-[^ ]*[.]fc44[.]noarch[.]rpm"
check fedora-45 "--nightly" 'dnf install -y .*/hermes-[0-9][^ ]*[.]fc45[.]x86_64[.]rpm' "dnf install -y .*/${kms_core}-0[.]4[.]1-[^ ]*[.]noarch[.]rpm"

if [ "$failures" -ne 0 ]; then
  echo "$failures dry-run check(s) failed." >&2
  exit 1
fi
echo "Installer dry runs passed."
