#!/usr/bin/env bash
# Report feed mistakes before publication instead of silently dropping entries.
set -euo pipefail
usage() {
    printf 'usage: %s FEED.json\n' "$0"
}
if [[ ${1-} == -h || ${1-} == --help ]]; then
    usage
    exit 0
fi
if (( $# != 1 )); then
    usage >&2
    exit 2
fi
path=$1
if [[ ! -r $path || ! -f $path ]]; then
    printf 'cannot read %s\n' "$path" >&2
    exit 1
fi
rules="${BASH_SOURCE[0]%/*}/feed_lint.jq"
if ! report=$(TZ=UTC jq -ces -f "$rules" -- "$path"); then
    printf '%s is not a valid JSON feed\n' "$path" >&2
    exit 1
fi
count=$(jq -r '.errors | length' <<< "$report")
if (( count )); then
    jq -r '.errors[]' <<< "$report" >&2
    printf '\n%s problem(s) found in %s\n' "$count" "$path" >&2
    exit 1
fi
printf '%s: OK (%s message(s))\n' "$path" "$(jq -r '.count' <<< "$report")"
