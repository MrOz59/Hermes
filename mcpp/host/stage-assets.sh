#!/usr/bin/env bash
# Build Hermes's Web UI and stage the Linux runtime assets.
set -euo pipefail
if (( $# != 4 )); then
    echo "usage: $0 REPOSITORY ASSETS STAMP DEPFILE" >&2
    exit 2
fi
source "${BASH_SOURCE[0]%/*}/depfile.sh"

source_dir="$(realpath -- "$1")/src_assets"
output=$2 stamp=$3 depfile=$4
common="$source_dir/common/assets"
web="$common/web"
linux="$source_dir/linux/assets"
compositors="$source_dir/linux/misc/session-compositors"
mkdir -p -- "$(dirname -- "$output")"
work=$(mktemp -d -- "$(dirname -- "$output")/hermes-assets-XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
assets="$work/assets"
mkdir -- "$assets"
tar -C "$common" --dereference --exclude=web -cf - . | tar -C "$assets" -xf -
cp -RLp -- "$linux/." "$assets/"
cp -RLp -- "$compositors" "$assets/session-compositors"
(
    cd "$web"
    HERMES_ASSETS_DIR="$(realpath -- "$work")" bun run build
)
rm -rf -- "$output"
mv -- "$assets" "$output"

# Directory mtimes also detect newly added or renamed files.
{
    depfile_word "$stamp"
    printf ':'
    while IFS= read -r -d '' path; do
        printf ' '
        depfile_word "$path"
    done < <(
        find "$common" "$linux" "$compositors" \
            \( -path "$web/node_modules" -o -path "$web/tests" \) -prune -o -print0 |
            LC_ALL=C sort -z
    )
    printf '\n'
} > "$depfile"
