#!/usr/bin/env bash
#
# publish-feed.sh — edit and publish the Hermes announcement feed.
#
# The feed is a single feed.json on the orphan `feed` branch, deliberately kept
# off `main`: publishing an announcement must not run CI, must not force-move
# the `nightly` tag, and must not make every installation report that a new
# nightly build is available.
#
# The Web UI reads it from
#   https://raw.githubusercontent.com/<owner>/<repo>/feed/feed.json
# so a push is live within a few minutes (raw.githubusercontent.com caches
# briefly). The format is documented in docs/announcements.md and enforced by
# scripts/feed_lint.py.
#
# Usage:
#   scripts/publish-feed.sh              # edit the live feed in $EDITOR, then publish
#   scripts/publish-feed.sh --show       # print the live feed and exit
#   scripts/publish-feed.sh --lint FILE  # validate a local file without publishing
#   scripts/publish-feed.sh --dry-run    # edit and validate, but do not push
#
# Environment:
#   HERMES_FEED_REMOTE   git remote to publish to (default: origin)
#   HERMES_FEED_BRANCH   branch holding feed.json (default: feed)
#   EDITOR               editor to open (default: vi)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

REMOTE="${HERMES_FEED_REMOTE:-origin}"
BRANCH="${HERMES_FEED_BRANCH:-feed}"
TEMPLATE="docs/feed/feed.example.json"
WORKFLOW_TEMPLATE="docs/feed/lint-feed.workflow.yml"
LINTER="scripts/feed_lint.py"

WORKTREE=""

usage() {
    sed -n '3,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

cleanup() {
    if [[ -n "$WORKTREE" && -d "$WORKTREE" ]]; then
        git worktree remove --force "$WORKTREE" >/dev/null 2>&1 || rm -rf "$WORKTREE"
    fi
}
trap cleanup EXIT

# Check out the feed branch into a throwaway worktree, creating the branch from
# the template the first time. A worktree reuses the local object store, so this
# costs nothing even though the branch shares no history with main.
checkout_feed() {
    WORKTREE="$(mktemp -d)"
    if git ls-remote --exit-code --heads "$REMOTE" "$BRANCH" >/dev/null 2>&1; then
        git fetch --quiet "$REMOTE" "$BRANCH"
        # -B: the published branch is authoritative; a stale local copy is reset.
        git worktree add --quiet -B "$BRANCH" "$WORKTREE" "refs/remotes/$REMOTE/$BRANCH"
    else
        echo "Branch '$BRANCH' does not exist on '$REMOTE' yet — creating it from $TEMPLATE." >&2
        git worktree add --quiet --detach "$WORKTREE" HEAD
        git -C "$WORKTREE" checkout --quiet --orphan "$BRANCH"
        git -C "$WORKTREE" rm -rqf . >/dev/null
        cp "$REPO_ROOT/$TEMPLATE" "$WORKTREE/feed.json"
        # Ship the branch's own lint workflow, so an edit made in the GitHub web
        # UI is validated too — that path never runs this script.
        mkdir -p "$WORKTREE/.github/workflows"
        cp "$REPO_ROOT/$WORKFLOW_TEMPLATE" "$WORKTREE/.github/workflows/lint-feed.yml"
        git -C "$WORKTREE" add .github/workflows/lint-feed.yml
    fi
}

DRY_RUN=0
case "${1-}" in
    -h|--help)
        usage
        exit 0
        ;;
    --lint)
        [[ $# -eq 2 ]] || { echo "--lint needs a file path" >&2; exit 2; }
        exec python3 "$LINTER" "$2"
        ;;
    --show)
        # Read-only: no worktree, so this never touches a local branch.
        if ! git ls-remote --exit-code --heads "$REMOTE" "$BRANCH" >/dev/null 2>&1; then
            echo "Branch '$BRANCH' does not exist on '$REMOTE' yet — nothing published." >&2
            exit 1
        fi
        git fetch --quiet "$REMOTE" "$BRANCH"
        git show "refs/remotes/$REMOTE/$BRANCH:feed.json"
        exit 0
        ;;
    --dry-run)
        DRY_RUN=1
        ;;
    "")
        ;;
    *)
        echo "unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
esac

checkout_feed
FEED="$WORKTREE/feed.json"

"${EDITOR:-vi}" "$FEED"

# Validate before committing: the Web UI drops entries it cannot parse without
# reporting anything, so a bad edit would look like a missing announcement.
python3 "$LINTER" "$FEED"

if git -C "$WORKTREE" diff --quiet -- feed.json && git -C "$WORKTREE" diff --cached --quiet -- feed.json; then
    if [[ -n "$(git -C "$WORKTREE" status --porcelain -- feed.json)" ]]; then
        : # new file on a freshly created branch
    else
        echo "No changes to publish."
        exit 0
    fi
fi

git -C "$WORKTREE" add feed.json
git -C "$WORKTREE" --no-pager diff --cached --stat

if [[ $DRY_RUN -eq 1 ]]; then
    echo "--dry-run: not committing or pushing."
    exit 0
fi

git -C "$WORKTREE" commit --quiet -m "feed: update announcements"
git -C "$WORKTREE" push --quiet "$REMOTE" "$BRANCH"
echo "Published to $REMOTE/$BRANCH."
echo "Live at https://raw.githubusercontent.com/MrOz59/Hermes/$BRANCH/feed.json within a few minutes."
