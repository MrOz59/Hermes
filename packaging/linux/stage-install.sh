#!/usr/bin/env bash
# Stage a Hermes install tree for a distribution package.
#
# Both build systems feed the same payload checks. The distribution package
# jobs add their own dependency metadata and installation hooks afterward.
#
# Usage: stage-install.sh <cmake-build-dir> <staging-dir>
#        stage-install.sh --mcpp <staging-dir> --result target/.last-result.json
set -euo pipefail

if [[ ${1:-} == --mcpp ]]; then
  shift
  root=${1:?usage: stage-install.sh --mcpp <staging-dir> --result target/.last-result.json}
  python3 "$(dirname "${BASH_SOURCE[0]}")/stage-mcpp-install.py" "$@"
else
  build_dir=${1:?usage: stage-install.sh <cmake-build-dir> <staging-dir>}
  root=${2:?usage: stage-install.sh <cmake-build-dir> <staging-dir>}

  rm -rf "$root"
  mkdir -p "$root"
  DESTDIR="$root" cmake --install "$build_dir"

  # Native packages expose the fork's binary and systemd unit names.
  rm -f "$root/usr/bin/sunshine"
  mv "$root"/usr/bin/sunshine-* "$root/usr/bin/hermes"
  if [ -f "$root/usr/lib/systemd/user/sunshine.service" ]; then
    mv "$root/usr/lib/systemd/user/sunshine.service" "$root/usr/lib/systemd/user/hermes.service"
  fi
fi

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
  # The usual cause for the unit, the udev rule and sysusers: CMake looks for
  # systemd.pc and udev.pc, which live in systemd-dev on Debian and Ubuntu and
  # in systemd-devel on Fedora, and installs none of them when they are absent.
  printf 'stage-install: check that the build found systemd and udev through pkg-config\n' >&2
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
