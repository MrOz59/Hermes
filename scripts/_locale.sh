#!/usr/bin/env bash
# Extract and maintain the native gettext catalogs. Borrowed from RetroArcher.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
catalog="$root/locale/hermes.po"
locales=(bg cs de en en_GB en_US es fr it ja ko pl pt pt_BR ru sv tr uk zh zh_TW)
extract=false
init=false
update=false
compile=false
for flag; do
    case "$flag" in
        --extract) extract=true ;;
        --init) init=true ;;
        --update) update=true ;;
        --compile) compile=true ;;
        -h|--help) echo 'usage: _locale.sh [--extract] [--init] [--update] [--compile]'; exit 0 ;;
        *) printf 'unknown option: %s\n' "$flag" >&2; exit 2 ;;
    esac
done

if $extract; then
    mkdir -p "$root/locale"
    mapfile -d '' files < <(find "$root/src" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.m' -o -name '*.mm' \) -print0 | sort -z)
    xgettext --keyword=translate:1,1t --keyword=translate:1c,2,2t \
        --keyword=translate:1,2,3t --keyword=translate:1c,2,3,4t \
        --keyword=gettext:1 --keyword=pgettext:1c,2 \
        --keyword=ngettext:1,2 --keyword=npgettext:1c,2,3 \
        --default-domain=hermes --output="$catalog" --language=C++ --boost \
        --from-code=utf-8 -F --msgid-bugs-address=https://github.com/MrOz59/Hermes/issues \
        --copyright-holder=MrOz59 --package-name=Hermes --package-version=v0 "${files[@]}"
    if [[ -f "$catalog" ]]; then
        sed -i '/^"Language: \\n"$/d
            s/^# SOME DESCRIPTIVE TITLE\.$/# Translations template for Hermes./
            /^#/s/YEAR/'"$(date +%Y)"'/
            /^#/s/PACKAGE/Hermes/' "$catalog"
    fi
fi

if $init; then
    for locale in "${locales[@]}"; do
        directory="$root/locale/$locale"
        [[ ! -d "$directory" ]] || continue
        mkdir -p "$directory/LC_MESSAGES"
        msginit --no-translator --locale="$locale" --input="$catalog" \
            --output-file="$directory/LC_MESSAGES/hermes.po"
    done
fi

if $update || $compile; then
    shopt -s nullglob
    for po in "$root"/locale/*/LC_MESSAGES/hermes.po; do
        if $update; then msgmerge --update --backup=none "$po" "$catalog"; fi
        if $compile; then msgfmt --check --output-file="${po%.po}.mo" "$po"; fi
    done
fi
