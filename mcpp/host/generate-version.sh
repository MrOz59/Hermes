#!/usr/bin/env bash
# Generate the version macros consumed by Hermes's C++ sources.
set -euo pipefail
if (( $# < 3 || $# > 4 )); then
    echo "usage: $0 REPOSITORY HEADER DEPFILE [PYROWAVE_LIBRARY]" >&2
    exit 2
fi
source "${BASH_SOURCE[0]%/*}/depfile.sh"

root=$1 header=$2 depfile=$3 library=${4-}
version=$(<"$root/VERSION")
version=${version#"${version%%[![:space:]]*}"}
version=${version%"${version##*[![:space:]]}"}
commit=${COMMIT-}
dependencies=("$root/VERSION")
git_at_root() {
    GIT_OPTIONAL_LOCKS=0 git -C "$root" "$@"
}
if [[ -n ${HERMES_BUILD_HAS_BRANCH-} && -n ${HERMES_BUILD_HAS_BUILD_VERSION-} ]]; then
    if [[ -n ${BUILD_VERSION-} ]]; then
        version=${BUILD_VERSION#v}
    fi
elif command -v git >/dev/null; then
    commit=$(git_at_root rev-parse HEAD 2>/dev/null) || commit=
    if branch=$(git_at_root rev-parse --abbrev-ref HEAD 2>/dev/null); then
        if [[ $branch != main && $branch != master ]]; then
            version+=".$(git_at_root rev-parse --short HEAD)"
        fi
        if ! git_at_root diff -b --quiet --exit-code; then
            version+=.dirty
        fi
        while IFS= read -r -d '' path; do
            dependencies+=("$root/$path")
        done < <(git_at_root ls-files -z)
        ref=$(git_at_root symbolic-ref -q HEAD) || ref=
        for name in HEAD index packed-refs "$ref"; do
            [[ -n $name ]] || continue
            path=$(git_at_root rev-parse --git-path "$name")
            [[ $path == /* ]] || path="$root/$path"
            dependencies+=("$path")
        done
    fi
fi
if [[ ! $version =~ ([0-9]+)\.([0-9]+)\.([0-9]+) ]]; then
    printf 'invalid Hermes version %q\n' "$version" >&2
    exit 1
fi
major=${BASH_REMATCH[1]} minor=${BASH_REMATCH[2]} patch=${BASH_REMATCH[3]}

values=(PROJECT_NAME Hermes PROJECT_VERSION "$version" PROJECT_VERSION_MAJOR "$major"
    PROJECT_VERSION_MINOR "$minor" PROJECT_VERSION_PATCH "$patch" PROJECT_VERSION_COMMIT "${commit:-unknown}"
    HERMES_PUBLISHER_NAME 'Hermes contributors'
    HERMES_PUBLISHER_WEBSITE https://github.com/MrOz59/Hermes
    HERMES_PUBLISHER_ISSUE_URL https://github.com/MrOz59/Hermes/issues)
if [[ -n $library ]]; then
    checksum=$(sha256sum < "$library")
    values+=(HERMES_BUILD_PYROWAVE_PREFIX "$(dirname -- "$(dirname -- "$library")")"
        HERMES_BUILD_PYROWAVE_SHA256 "${checksum%% *}")
fi
content=$(jq -nr --args '
    "#pragma once",
    (range(0; ($ARGS.positional | length); 2) as $index |
     "#define \($ARGS.positional[$index]) \($ARGS.positional[$index + 1] | @json)")
' -- "${values[@]}")
if [[ ! -f $header ]] || ! cmp -s -- "$header" <(printf '%s\n' "$content"); then
    printf '%s\n' "$content" > "$header"
fi

{
    depfile_word "$header"
    printf ':'
    while IFS= read -r -d '' path; do
        [[ -f $path ]] || continue
        printf ' '
        depfile_word "$path"
    done < <(printf '%s\0' "${dependencies[@]}" | LC_ALL=C sort -zu)
    printf '\n'
} > "$depfile"
