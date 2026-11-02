#!/usr/bin/env bash
# Offline installer helper checks: no downloads, root commands or packages.
set -euo pipefail

script="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/install.sh"
# Load function definitions without executing the final main "$@" entrypoint.
source <(sed '$d' "$script")

workdir="$(mktemp -d -t hermes-installer-test.XXXXXX)"
trap 'rm -rf -- "$workdir"' EXIT
mkdir -p "$workdir/remote" "$workdir/downloaded"
DRY_RUN=0

# Test GitHub release selection using local URLs with realistic asset names.
filename="hermes-kms-dkms_0.4.1-1_all.deb"
printf 'representative DKMS package content\n' > "$workdir/remote/$filename"
(
  cd "$workdir/remote"
  sha256sum "$filename" > SHA256SUMS
)
assets="file://$workdir/remote/$filename
file://$workdir/remote/SHA256SUMS"
url="$(release_asset_url "$assets" '/hermes-kms-dkms_.*_all\.deb$')"
[ "$url" = "file://$workdir/remote/$filename" ]

# Replace network calls with local copies; the checksum logic remains real.
download() {
  local from="${1#file://}"
  cp -- "$from" "$2"
}
download_checked_kms "$assets" "$url" "$workdir/downloaded/$filename"
cmp "$workdir/remote/$filename" "$workdir/downloaded/$filename"

# A modified package must be rejected rather than installed.
printf 'tampered\n' > "$workdir/remote/$filename"
if (download_checked_kms "$assets" "$url" "$workdir/downloaded/$filename" >/dev/null 2>&1); then
  echo "FAIL: tampered download passed checksum verification" >&2
  exit 1
fi

# DKMS must match both the selected version AND the running kernel.
dkms() {
  printf 'hermes-kms/0.4.1, %s, x86_64: installed\n' "$(uname -r)"
}
has_kms_installed
dkms() {
  printf 'hermes-kms/0.4.0, %s, x86_64: installed\n' "$(uname -r)"
}
if has_kms_installed; then
  echo "FAIL: old installed DKMS version was treated as compatible" >&2
  exit 1
fi
echo "Installer offline helper tests passed."
