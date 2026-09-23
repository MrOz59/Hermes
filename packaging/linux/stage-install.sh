#!/usr/bin/env bash
# Stage a Hermes install tree for a distribution package.
#
# The .deb and .rpm used to be assembled by hand, file by file, and quietly
# shipped without the menu entries, the icons and (on Fedora) the systemd unit
# (#52): the desktop files and icons exist only as CMake install rules, which
# nothing in those jobs ran. This runs the install the Arch package already
# uses, applies the two renames a package needs, and refuses to hand back a
# tree that is missing anything a user would notice.
#
# Usage: stage-install.sh <build-dir> <staging-dir>
set -euo pipefail

build_dir=${1:?usage: stage-install.sh <build-dir> <staging-dir>}
root=${2:?usage: stage-install.sh <build-dir> <staging-dir>}

rm -rf "$root"
mkdir -p "$root"
DESTDIR="$root" cmake --install "$build_dir"

# CMake installs the versioned binary plus a `sunshine` symlink beside it; a
# package ships one binary, named after this fork.
rm -f "$root/usr/bin/sunshine"
mv "$root"/usr/bin/sunshine-* "$root/usr/bin/hermes"

# Same for the unit, whose upstream name would collide with an apollo or
# sunshine install on the same machine.
if [ -f "$root/usr/lib/systemd/user/sunshine.service" ]; then
  mv "$root/usr/lib/systemd/user/sunshine.service" "$root/usr/lib/systemd/user/hermes.service"
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
