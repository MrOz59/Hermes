#!/usr/bin/env bash
#
# bump-version.sh — bump the Hermes version in one step.
#
# The top-level VERSION file is the single source of truth. This script
# bumps it, mirrors the value into package.json, PKGBUILD (resetting pkgrel
# to 1) and the Hermes mcpp manifests, rolls the CHANGELOG's [Unreleased]
# section into a dated release section, then creates a commit and an
# annotated git tag (vX.Y.Z).
#
# Usage:
#   scripts/bump-version.sh patch        # 0.1.0 -> 0.1.1
#   scripts/bump-version.sh minor        # 0.1.1 -> 0.2.0
#   scripts/bump-version.sh major        # 0.2.0 -> 1.0.0
#   scripts/bump-version.sh 1.4.2        # set an explicit version
#
# Options:
#   --no-tag      Update files and commit, but do not create the git tag.
#   --no-commit   Update files only (implies --no-tag); leaves them staged.
#   -h, --help    Show this help.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION_FILE="VERSION"
PKG_JSON="package.json"
CHANGELOG="CHANGELOG.md"
PKGBUILD="PKGBUILD"
# Workspace members inherit the root version. The other packages below
# track the Hermes release but are built only as dependencies or CI tools.
MCPP_MANIFESTS=(
  mcpp.toml
  mcpp/kwin-transport/mcpp.toml
  mcpp/hid-consumer/mcpp.toml
  mcpp/libvirtualhid-tests/mcpp.toml
  mcpp/libvirtualhid-sdl3-consumer/mcpp.toml
)

do_commit=1
do_tag=1
bump=""

usage() { sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

for arg in "$@"; do
  case "$arg" in
    -h|--help) usage; exit 0 ;;
    --no-tag) do_tag=0 ;;
    --no-commit) do_commit=0; do_tag=0 ;;
    major|minor|patch) bump="$arg" ;;
    [0-9]*.[0-9]*.[0-9]*) bump="$arg" ;;
    *) echo "error: unknown argument '$arg'" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "$bump" ]]; then
  echo "error: specify major|minor|patch or an explicit X.Y.Z version" >&2
  usage
  exit 1
fi

current="$(tr -d '[:space:]' < "$VERSION_FILE")"
if ! [[ "$current" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "error: $VERSION_FILE must contain an X.Y.Z version" >&2
  exit 1
fi
IFS='.' read -r major minor patch <<< "$current"

case "$bump" in
  major) new="$((major + 1)).0.0" ;;
  minor) new="${major}.$((minor + 1)).0" ;;
  patch) new="${major}.${minor}.$((patch + 1))" ;;
  *)     new="$bump" ;;
esac

if ! [[ "$new" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "error: resulting version '$new' is not valid SemVer (X.Y.Z)" >&2
  exit 1
fi

mcpp_package_version() {
  local manifest=$1 replacement=${2:-}
  local section='\[package\]' address
  if [[ "$manifest" == mcpp.toml ]]; then
    section='\[workspace\.package\]'
  fi
  address="/^[[:blank:]]*${section}[[:blank:]]*(#.*)?\$/,/^[[:blank:]]*\[/"
  local field='^([[:blank:]]*version[[:blank:]]*=[[:blank:]]*")([^"]+)("[[:blank:]]*(#.*)?)$'
  if [[ -z "$replacement" ]]; then
    sed -nE "${address}s/${field}/\\2/p" "$manifest"
  else
    sed -i -E "${address}s/${field}/\\1${replacement}\\3/" "$manifest"
  fi
}

for manifest in "${MCPP_MANIFESTS[@]}"; do
  if ! found="$(mcpp_package_version "$manifest")" || [[ "$found" != "$current" ]]; then
    echo "error: expected one package version matching $VERSION_FILE ($current) in $manifest" >&2
    exit 1
  fi
done

echo "Bumping version: $current -> $new"
release_files=("$VERSION_FILE" "${MCPP_MANIFESTS[@]}")

# 1. VERSION (single source of truth)
printf '%s\n' "$new" > "$VERSION_FILE"

# 2. package.json (top-level "version" field)
if [[ -f "$PKG_JSON" ]]; then
  # Only touch the first top-level "version": "..." line.
  sed -i -E "0,/^([[:space:]]*\"version\":[[:space:]]*)\"[^\"]*\"/s//\1\"$new\"/" "$PKG_JSON"
  release_files+=("$PKG_JSON")
fi

# 3. PKGBUILD — the Arch pkgver drives BUILD_VERSION, which the build embeds
#    into the binary and the web UI shows. Keep it in lockstep with VERSION and
#    reset pkgrel to 1 for the new upstream version.
if [[ -f "$PKGBUILD" ]]; then
  sed -i -E "0,/^pkgver=.*/s//pkgver=$new/" "$PKGBUILD"
  sed -i -E "0,/^pkgrel=.*/s//pkgrel=1/" "$PKGBUILD"
  release_files+=("$PKGBUILD")
fi

# 4. mcpp package metadata
for manifest in "${MCPP_MANIFESTS[@]}"; do
  mcpp_package_version "$manifest" "$new"
done

# 5. CHANGELOG.md — turn [Unreleased] into a dated release section and
#    open a fresh [Unreleased] above it.
if [[ -f "$CHANGELOG" ]]; then
  today="$(date +%Y-%m-%d)"
  if grep -q "^## \[Unreleased\]" "$CHANGELOG"; then
    sed -i -E "s|^## \[Unreleased\].*$|## [Unreleased]\n\n## [$new] - $today|" "$CHANGELOG"
    release_files+=("$CHANGELOG")
  else
    echo "warning: no '## [Unreleased]' heading in $CHANGELOG; skipping changelog roll" >&2
  fi
fi

git add -- "${release_files[@]}"

if [[ "$do_commit" -eq 0 ]]; then
  echo "Files updated and staged (no commit). Done."
  exit 0
fi

git commit -m "Release v$new"
echo "Committed: Release v$new"

if [[ "$do_tag" -eq 1 ]]; then
  git tag -a "v$new" -m "Hermes v$new"
  echo "Tagged: v$new"
  echo
  echo "Next: git push origin HEAD && git push origin v$new"
else
  echo "Next: git push origin HEAD   (tag skipped)"
fi
