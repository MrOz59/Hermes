#!/usr/bin/env bash
# Render the published Doxygen site with Hermes sources and the vendored theme.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
theme="$repo/third-party/doxyconfig"
web="$repo/src_assets/common/assets/web"
export READTHEDOCS_OUTPUT="$(realpath -m "${READTHEDOCS_OUTPUT:-$repo/target/docs}")"
export READTHEDOCS_VERSION="${READTHEDOCS_VERSION:-$(<"$repo/VERSION")}"
html="$READTHEDOCS_OUTPUT/html"
mkdir -p "$repo/target" "$html"
work="$(mktemp -d "$repo/target/docs-build.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT

bun_version="$(jq -er '.packageManager | capture("^bun@(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)$").version' "$web/package.json")"
if ! command -v bun >/dev/null || [[ $(bun --version) != "$bun_version" ]]; then
    curl --fail --location --retry 3 \
        "https://github.com/oven-sh/bun/releases/download/bun-v${bun_version}/bun-linux-x64.zip" \
        --output "$work/bun.zip"
    unzip -q "$work/bun.zip" -d "$work"
    chmod +x "$work/bun-linux-x64/bun"
    export PATH="$work/bun-linux-x64:$PATH"
fi
bun run --cwd "$web" docs:assets

# Install only the vendored theme packages, using its unchanged npm lockfile.
mkdir "$work/vendor"
cp -- "$theme/package.json" "$theme/package-lock.json" "$work/vendor"
bun install --cwd "$work/vendor" --frozen-lockfile --ignore-scripts
dependencies="$work/vendor/node_modules"
fontawesome="$dependencies/@fortawesome/fontawesome-free"
shared_web="$dependencies/@lizardbyte/shared-web/dist"
mkdir -p "$html/assets/fontawesome"/{css,js} "$html/assets/shared-web"
cp -- "$fontawesome/css/all.min.css" "$html/assets/fontawesome/css/"
cp -- "$fontawesome/js/all.min.js" "$html/assets/fontawesome/js/"
cp -r -- "$fontawesome/webfonts" "$html/assets/fontawesome/"
cp -- "$shared_web/crowdin.js" "$shared_web/crowdin-doxygen-css.css" "$html/assets/shared-web/"

# Keep the vendored theme and Hermes overrides in the same order as Doxyconfig.
# Resolve its loose files in place instead of creating copies under docs/.
{
    while IFS= read -r line; do
        case "$line" in
            'HTML_HEADER = doxyconfig-header.html')
                printf 'HTML_HEADER = "%s/doxyconfig-header.html"\n' "$theme"
                ;;
            'HTML_EXTRA_FILES += doxyconfig-readthedocs-search.js')
                printf 'HTML_EXTRA_FILES += "%s/doxyconfig-readthedocs-search.js"\n' "$theme"
                ;;
            'HTML_EXTRA_STYLESHEET += doxyconfig.css')
                printf 'HTML_EXTRA_STYLESHEET += "%s/doxyconfig.css"\n' "$theme"
                ;;
            *) printf '%s\n' "$line" ;;
        esac
    done < "$theme/doxyconfig-Doxyfile"
    cat "$repo/docs/Doxyfile"
} > "$work/Doxyfile"
cd "$repo/docs"
doxygen "$work/Doxyfile"
test -s "$html/index.html"
