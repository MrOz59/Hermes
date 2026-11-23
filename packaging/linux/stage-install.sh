#!/usr/bin/env bash
# Stage a Hermes install tree for a native Linux distribution package.
set -euo pipefail
umask 022

usage() {
  printf '%s\n'     'usage: stage-install.sh --mcpp ROOT [--result FILE ... | --host FILE --assets DIR --version-header FILE]'     'Optional artifacts: --card-broker FILE --session-broker FILE --detached-session FILE --gamemode FILE'     'Optional payload settings: --tray --pyrowave-prefix DIR --libdir PATH'
}

fail() {
  printf 'stage-install: %s\n' "$*" >&2
  exit 1
}

if [[ ${1:-} != --mcpp ]]; then
  usage >&2
  exit 2
fi
shift
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  usage
  exit
fi
destination=${1:?pass the mcpp staging directory}
[[ $destination == /* ]] || destination=$PWD/$destination
shift

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fqdn=io.github.mroz59.Hermes
declare -a results=()
declare -A options=() artifacts=()
explicit=false
tray=false
while (( $# )); do
  option=${1%%=*}
  case "$option" in
    -h|--help) usage; exit ;;
    --tray) [[ $1 == --tray ]] || fail "invalid option: $1"; tray=true ;;
    --result|--host|--assets|--version-header|--card-broker|--session-broker|--detached-session|--gamemode|--pyrowave-prefix|--libdir)
      if [[ $1 == *=* ]]; then
        value=${1#*=}
      else
        shift
        (( $# )) || fail "missing value for $option"
        [[ $1 != --* ]] || fail "missing value for $option"
        value=$1
      fi
      case "$option" in
        --result) results+=("$value") ;;
        --pyrowave-prefix|--libdir) options[${option#--}]=$value ;;
        *) options[${option#--}]=$value; explicit=true ;;
      esac
      ;;
    --) (( $# == 1 )) || fail 'pass exactly one new staging directory'; break ;;
    *) fail "unexpected argument: $1" ;;
  esac
  shift
done

[[ ! -e $destination && ! -L $destination ]] || fail "staging directory already exists: $destination"
work=$(mktemp -d -- "$(dirname -- "$destination")/hermes-stage.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

if (( ${#results[@]} )); then
  $explicit && fail '--result cannot be combined with explicit artifact paths'
  host_output=
  for result in "${results[@]}"; do
    kind=$(jq -esr 'if length == 1 then .[0].kind | strings else error("expected one result object") end' < "$result")
    if [[ $kind == mcpp.last-result ]]; then
      resolved=$(jq -er '.data.result | strings | select(length > 0 and (contains("\u0000") | not))' < "$result") ||
        fail "invalid mcpp result pointer: $result"
      result=$resolved
    fi
    # Use NUL delimiters so artifact paths remain intact when passed to Bash.
    jq -sj '
      if length == 1 and .[0].kind == "mcpp.result" and .[0].data.status == "ok"
          and (.[0].data.artifacts | type) == "array" then
        .[0].data.artifacts[] |
        if type != "object" or (.kind | type) != "string" then error("invalid mcpp artifact")
        elif .kind == "bin" then
          if ([.name, .path] | all(type == "string" and length > 0 and (contains("\u0000") | not)))
          then .name, "\u0000", .path, "\u0000"
          else error("invalid mcpp binary name or path")
          end
        else empty end
      else error("no successful mcpp build") end
    ' < "$result" > "$work/artifacts" || fail "cannot read mcpp artifacts from $result"

    while IFS= read -r -d '' name && IFS= read -r -d '' path; do
      if [[ -n ${artifacts[$name]-} && ${artifacts[$name]} != "$path" ]]; then
        fail "conflicting mcpp results for $name"
      fi
      artifacts[$name]=$path
      if [[ $name == hermes ]]; then
        resolution="$(dirname -- "$result")/resolution.json"
        host_output=$(jq -esr '
          if length != 1 or (.[0].graph.packages | type) != "array" then error("invalid mcpp resolution")
          else first(.[0].graph.packages[] | select(.package.name == "hermes-host") | .outDir) |
            strings | select(length > 0 and (contains("\u0000") | not))
          end
        ' < "$resolution") || fail "no hermes-host output in $resolution"
      fi
    done < "$work/artifacts"
  done
  [[ -n $host_output ]] || fail 'the mcpp results did not include hermes-host'
  options[host]=${artifacts[hermes]}
  options[assets]=$host_output/assets
  options[version-header]=$host_output/hermes-build-version.h
  options[card-broker]=${artifacts[hermes-kms-card-broker]-}
  options[session-broker]=${artifacts[hermes-session-broker]-}
  options[detached-session]=${artifacts[hermes-detached-session]-}
  options[gamemode]=${artifacts[hermes-gamemode]-}
fi

host=${options[host]-}
assets=${options[assets]-}
version_header=${options[version-header]-}
[[ -n $host && -n $assets && -n $version_header ]] ||
  fail 'pass --result or supply --host, --assets and --version-header'
LC_ALL=C grep -aPq '/usr/share/hermes\x00' -- "$host" || fail 'build hermes-host with the package-assets feature'
for file in apps.json web/index.html session-compositors/weston.conf; do
  [[ -f $assets/$file ]] || fail "missing runtime asset $file in $assets"
done

declare -A metadata=()
while IFS= read -r line || [[ -n $line ]]; do
  if [[ $line =~ ^'#define '(PROJECT_VERSION|HERMES_BUILD_PYROWAVE_PREFIX|HERMES_BUILD_PYROWAVE_SHA256)' '(.+)$ ]]; then
    field=${BASH_REMATCH[1]}
    metadata[$field]=$(jq -esr '
      select(length == 1) | .[0] | strings | select(contains("\u0000") | not)
    ' <<< "${BASH_REMATCH[2]}") || fail "invalid $field in $version_header"
  fi
done < "$version_header"
version=${metadata[PROJECT_VERSION]-}
[[ -n $version ]] || fail "PROJECT_VERSION is missing from $version_header"
year=1990 month=01 day=01
if [[ $version =~ ^([0-9]{4})\.([0-9]{3,4})\.[0-9]+$ ]]; then
  year=${BASH_REMATCH[1]}
  date_digits=0${BASH_REMATCH[2]}
  day=${date_digits: -2}
  month=${date_digits: -4:2}
fi

templates=$repo/packaging/linux
misc=$repo/src_assets/linux/misc
brand_icon=$repo/src_assets/common/branding/hermes.svg
root=$work/payload
binaries=$root/usr/bin
share=$root/usr/share
data=$share/hermes
icons=$share/icons/hicolor/scalable

pkg_directory() {
  local path
  path=$(pkg-config --variable="$2" "$1") || fail "cannot query $1.$2 with pkg-config"
  if [[ $path != /usr && $path != /usr/* ]] || [[ /$path/ == */../* ]]; then
    fail "$1.$2 must be a directory under /usr"
  fi
  printf '%s\n' "$root$path"
}

udev=$(pkg_directory udev udev_dir)/rules.d
user_units=$(pkg_directory systemd systemd_user_unit_dir)
system_units=$(pkg_directory systemd systemd_system_unit_dir)
sysusers=$(pkg_directory systemd sysusers_dir)
modules=$(pkg_directory systemd modules_load_dir)

render() {
  local source=$1 target=$2
  mkdir -p -- "$(dirname -- "$target")"
  jq -nj --rawfile template "$templates/$source" \
    --arg version "$version" --arg year "$year" --arg month "$month" --arg day "$day" --arg fqdn "$fqdn" '
      {
        HERMES_DATAROOTDIR: "/usr/share",
        PROJECT_BRIEF_DESCRIPTION: "GameStream host for Hestia",
        PROJECT_DAY: $day,
        PROJECT_DESCRIPTION: "Self-hosted game stream host for Hestia and Artemis",
        PROJECT_FQDN: $fqdn,
        PROJECT_HOMEPAGE_URL: "https://github.com/MrOz59/Hermes",
        PROJECT_LICENSE: "GPL-3.0-only",
        PROJECT_LONG_DESCRIPTION:
          ("Offering low latency, cloud gaming server capabilities with support for AMD, Intel, " +
           "and Nvidia GPUs for hardware encoding. Software encoding is also available. You can " +
           "connect to Hermes from any Hestia or Artemis client on a variety of devices. A web UI " +
           "is provided to allow configuration, and client pairing, from your favorite web browser. " +
           "Pair from the local server or any mobile device."),
        PROJECT_MONTH: $month,
        PROJECT_NAME: "Hermes",
        PROJECT_VERSION: $version,
        PROJECT_YEAR: $year,
        HERMES_DESKTOP_ICON: "hermes",
        HERMES_SERVICE_START_COMMAND: "ExecStart=hermes",
        HERMES_SERVICE_STOP_COMMAND: ""
      } as $fields |
      $template | gsub("@(?<name>[A-Za-z_][A-Za-z0-9_]*)@"; $fields[.name] // error("unknown template field: " + .name))
    ' > "$target"
}

stage_pyrowave() {
  local requested=${options[pyrowave-prefix]-} libdir=${options[libdir]-}
  local linked=${metadata[HERMES_BUILD_PYROWAVE_PREFIX]-}
  local dynamic dependency prefix expected runpath rpath patchelf source runtime hash target file link
  dynamic=$(LC_ALL=C readelf -d -- "$host")
  dependency=$(sed -nE '/\(NEEDED\).*\[libpyrowave-shared\.so/{s/.*\[(libpyrowave-shared\.so[^]]*)\].*/\1/p;q;}' <<< "$dynamic")
  if [[ -z $dependency ]]; then
    [[ -z $requested && -z $libdir ]] || fail 'Hermes was built without shared PyroWave'
    return
  fi
  [[ $dependency != */* ]] || fail "invalid PyroWave library name: $dependency"
  [[ -n $libdir ]] || fail 'pass --libdir to stage the linked PyroWave runtime'
  [[ $libdir != /* && $libdir != . && /$libdir/ != */../* ]] || fail '--libdir must be relative to /usr'
  expected=$(realpath -ms -- "/usr/$libdir/hermes")
  runpath=$(sed -nE '/\(RUNPATH\)/{s/.*\[([^]]*)\].*/\1/p;q;}' <<< "$dynamic")
  rpath=$(sed -nE '/\(RPATH\)/{s/.*\[([^]]*)\].*/\1/p;q;}' <<< "$dynamic")
  if [[ -z $runpath && $rpath == "$expected" ]]; then
    # Convert legacy ELF dtags only on the staged copy.
    patchelf=$repo/.toolchain/patchelf-0.18.0/bin/patchelf
    [[ -f $patchelf ]] || patchelf=patchelf
    "$patchelf" --set-rpath "$expected" "$binaries/hermes"
    dynamic=$(LC_ALL=C readelf -d -- "$binaries/hermes")
    runpath=$(sed -nE '/\(RUNPATH\)/{s/.*\[([^]]*)\].*/\1/p;q;}' <<< "$dynamic")
  fi
  [[ $runpath == "$expected" ]] || fail "Hermes must have install RUNPATH $expected"

  requested=${requested:-${HERMES_MCPP_PYROWAVE_PREFIX:-}}
  if [[ -n $requested ]]; then
    [[ $requested == /* ]] || requested=$repo/$requested
    requested=$(realpath -m -- "$requested")
    if [[ -n $linked && $linked != "$requested" ]]; then
      fail "PyroWave stage prefix differs from the host-linked SDK at $linked"
    fi
  fi
  prefix=${linked:-${requested:-$repo/target/pyrowave-sdk/install}}
  source=$prefix/lib
  runtime=$source/$dependency
  [[ -f $runtime ]] || fail "PyroWave SDK at $prefix lacks $dependency"
  if [[ -n $linked ]]; then
    hash=$(sha256sum < "$runtime")
    [[ ${hash%% *} == "${metadata[HERMES_BUILD_PYROWAVE_SHA256]-}" ]] ||
      fail "PyroWave runtime differs from the library used to link Hermes: $runtime"
  fi
  target=$root$expected
  mkdir -p -- "$target"
  for file in "$source"/libpyrowave-shared.so*; do
    if [[ -L $file ]]; then
      link=$(readlink -- "$file")
      [[ $link != */* && -f $source/$link ]] || fail "invalid PyroWave library symlink: $file"
      ln -s -- "$link" "$target/${file##*/}"
    else
      install -m 644 -- "$file" "$target/${file##*/}"
    fi
  done
  if [[ -d $prefix/share/licenses/pyrowave ]]; then
    for file in "$prefix"/share/licenses/pyrowave/*; do
      install -Dm 644 -- "$file" "$share/licenses/hermes/pyrowave/${file##*/}"
    done
  fi
}

mkdir -p -- "$share"
cp -RL -- "$assets" "$data"
find "$data" -type f -exec chmod 644 {} +
install -Dm 755 -- "$host" "$binaries/hermes"
for name in hermes-gamescope-launch hermes-monitor-recovery; do
  install -m 755 -- "$misc/$name" "$binaries/$name"
done
for allow in card-broker session-broker; do
  binary=${options[$allow]-}
  [[ -n $binary ]] || continue
  name=hermes-$allow
  [[ $allow != card-broker ]] || name=hermes-kms-card-broker
  install -m 755 -- "$binary" "$binaries/$name"
  install -m 644 -- "$misc/$allow.allow" "$data/$allow.allow.example"
  for extension in socket service; do
    unit=$name.$extension
    install -Dm 644 -- "$misc/$unit" "$system_units/$unit"
  done
done

if [[ -n ${options[detached-session]-} ]]; then
  install -m 755 -- "${options[detached-session]}" "$binaries/hermes-detached-session"
fi
if [[ -n ${options[gamemode]-} ]]; then
  install -m 755 -- "${options[gamemode]}" "$binaries/hermes-gamemode"
  desktop=$fqdn.GameMode.desktop
  render "desktop/$desktop" "$share/applications/$desktop"
fi
if $tray; then
  install -Dm 644 -- "$brand_icon" "$icons/status/hermes-tray.svg"
  for name in hermes-playing.svg hermes-pausing.svg hermes-locked.svg; do
    install -m 644 -- "$repo/src_assets/common/assets/web/public/images/$name" "$icons/status/$name"
  done
fi

for name in 60-hermes.rules 74-hermes-session-acl.rules 99-hermes-session-input.rules; do
  install -Dm 644 -- "$misc/$name" "$udev/$name"
done
install -Dm 644 -- "$misc/10-hermes-session-deny.rules" "$share/polkit-1/rules.d/10-hermes-session-deny.rules"
install -Dm 644 -- "$misc/hermes-sysusers.conf" "$sysusers/hermes.conf"
for name in 60-hermes.conf 61-hermes-evdi.conf; do
  install -Dm 644 -- "$misc/$name" "$modules/$name"
done
install -Dm 644 -- "$misc/hermes-evdi-modprobe.conf" "$(dirname -- "$modules")/modprobe.d/hermes-evdi.conf"
install -Dm 644 -- "$brand_icon" "$icons/apps/hermes.svg"
for desktop in "$fqdn.desktop" "$fqdn.terminal.desktop"; do
  render "desktop/$desktop" "$share/applications/$desktop"
done
render "desktop/$fqdn.metainfo.xml" "$share/metainfo/$fqdn.metainfo.xml"
render systemd/hermes.service.in "$user_units/hermes.service"
shopt -s nullglob dotglob
stage_pyrowave
find "$root" -type d -exec chmod 755 {} +

# Verify the essential installed paths before publishing the tree atomically.
required=(
  usr/bin/hermes
  usr/lib/systemd/user/hermes.service
  "usr/share/applications/$fqdn.desktop"
  "usr/share/applications/$fqdn.terminal.desktop"
  usr/share/icons/hicolor/scalable/apps/hermes.svg
  "usr/share/metainfo/$fqdn.metainfo.xml"
  usr/share/hermes/apps.json
  usr/share/hermes/web/index.html
)
missing=()
for path in "${required[@]}"; do
  [[ -e $root/$path ]] || missing+=("$path")
done
if (( ${#missing[@]} )); then
  printf 'stage-install: the staged tree is missing %s\n' "${missing[*]}" >&2
  fail 'check that systemd and udev paths resolve through pkg-config'
fi
mv -nT -- "$root" "$destination"
[[ ! -e $root ]] || fail "staging directory already exists: $destination"

for path in \
  usr/bin/hermes-gamemode \
  "usr/share/applications/$fqdn.GameMode.desktop" \
  usr/share/icons/hicolor/scalable/status/hermes-tray.svg
do
  [[ -e $destination/$path ]] || printf 'stage-install: note: %s was not built\n' "$path" >&2
done
find "$destination" \( -type f -o -type l \) -printf '%P\n' | sort
