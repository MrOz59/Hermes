#!/usr/bin/env bash
# Stage a Hermes install tree for a distribution package.
#
# The distribution package jobs add dependency metadata and installation
# hooks after these payload checks.
#
# Usage: stage-install.sh --mcpp <staging-dir> --result target/.last-result.json
set -euo pipefail

if [[ ${1:-} != --mcpp ]]; then
  printf 'usage: stage-install.sh --mcpp <staging-dir> --result target/.last-result.json\n' >&2
  exit 2
fi
shift
root=${1:?pass the mcpp staging directory}
python3 "$(dirname "${BASH_SOURCE[0]}")/stage-mcpp-install.py" "$@"

# What the user sees if it is missing: no entry in the application menu, a
# generic icon in the tray, or a package that installs and then cannot be
# started at all.
required=(
  usr/bin/hermes
  usr/lib/systemd/user/hermes.service
  usr/share/applications/io.github.mroz59.Hermes.desktop
  usr/share/applications/io.github.mroz59.Hermes.terminal.desktop
  usr/share/icons/hicolor/scalable/apps/hermes.svg
  usr/share/metainfo/io.github.mroz59.Hermes.metainfo.xml
  usr/share/hermes/apps.json
  usr/share/hermes/web/index.html
)
missing=()
for path in "${required[@]}"; do
  [ -e "$root/$path" ] || missing+=("$path")
done
if [ ${#missing[@]} -ne 0 ]; then
  printf 'stage-install: the staged tree is missing %s\n' "${missing[*]}" >&2
  printf 'stage-install: check that systemd and udev paths resolve through pkg-config\n' >&2
  exit 1
fi

# Built only when their dependencies were found, so their absence is reported
# rather than fatal - it still changes what the package offers.
for path in \
  usr/bin/hermes-gamemode \
  usr/share/applications/io.github.mroz59.Hermes.GameMode.desktop \
  usr/share/icons/hicolor/scalable/status/hermes-tray.svg
do
  [ -e "$root/$path" ] || printf 'stage-install: note: %s was not built\n' "$path" >&2
done

find "$root" \( -type f -o -type l \) -printf '%P\n' | sort
