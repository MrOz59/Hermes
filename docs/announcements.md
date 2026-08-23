# Announcements

Hermes shows maintainer announcements at the top of the Web UI home page. The
intended use is a **known issue**: when something is broken and already being
worked on, the people hitting it see that on the home page instead of opening
another report.

An announcement is plain text, an optional link, and (usually) a dismiss
button. It never installs or changes anything on the host.

## Where the feed lives

A single `feed.json` on the repository's orphan `feed` branch, read by the
browser from:

```
https://raw.githubusercontent.com/MrOz59/Hermes/feed/feed.json
```

The branch is deliberately **not** `main`. A commit to `main` runs the whole CI
pipeline, force-moves the `nightly` tag and republishes the nightly release —
which would tell every installation that a new nightly is available. Publishing
a message must not do any of that.

Users can turn the feed off with the `announcements` option (see
[Configuration](configuration.md)). Turning it off stops the browser making the
request at all.

## Publishing

```bash
scripts/publish-feed.sh          # open the live feed in $EDITOR, validate, commit, push
scripts/publish-feed.sh --show   # print the live feed
scripts/publish-feed.sh --dry-run
```

The first run creates the `feed` branch, seeded with
`docs/feed/feed.example.json` and its own lint workflow. The script validates
before committing, because the Web UI silently drops entries it cannot parse — a
typo would look like a missing announcement, not an error.

You can equally edit `feed.json` directly in the GitHub web UI on the `feed`
branch. That path does not run this script, so the branch carries
`.github/workflows/lint-feed.yml`, which validates every push to it using
`scripts/feed_lint.py` from `main`: a broken edit fails the check instead of
quietly not showing up. Nothing else runs on that branch — no build, no nightly
tag move, no release.

Changes go live within a few minutes (`raw.githubusercontent.com` caches
briefly).

## Format

`docs/feed/feed.schema.json` is the authoritative schema; point your editor at
it via the `$schema` key. A message looks like:

```json
{
  "$schema": "https://raw.githubusercontent.com/MrOz59/Hermes/main/docs/feed/feed.schema.json",
  "version": 1,
  "messages": [
    {
      "id": "kms-cursor-rdna3",
      "level": "warning",
      "title": "Known issue: cursor missing on RDNA3 with Hermes-KMS",
      "body": "Already tracked — no need to open a new report.\nFixed in the current nightly.",
      "affects": ">=0.5.0 <0.5.2",
      "link": {
        "url": "https://github.com/MrOz59/Hermes/issues/42",
        "label": "Tracking issue"
      },
      "expires": "2026-12-01"
    }
  ]
}
```

| Field | Required | Notes |
|-------|----------|-------|
| `id` | yes | Lowercase slug. Dismissals are remembered per id — never reuse one for an unrelated message. |
| `title` | yes | One line. Plain text; markup is not rendered. |
| `body` | no | Detail. Line breaks are preserved; markup is not rendered. |
| `level` | no | `danger`, `warning`, `info` (default) or `success`. Also sets the order: `danger` first. |
| `channel` | no | `all` (default), `stable`, or `nightly`. A build counts as nightly when its version carries a suffix (`0.5.1+abc1234`). |
| `affects` | no | Version range. Clauses in one string are ANDed (`">=0.5.0 <0.5.2"`); a list is ORed. Omit for every version. |
| `link` | no | Absolute `http(s)` URL, or `{ "url": …, "label": … }`. Any other scheme is discarded by the client. |
| `starts` / `expires` | no | ISO 8601. Prefer `expires` over deleting an entry, so it retires on its own. |
| `dismissible` | no | `true` by default. Set `false` only for something that must stay put. |
| `revision` | no | Bump after editing to show the message again to people who already dismissed it. |

### Version ranges and nightlies

Build metadata is ignored when matching, so `"affects": "0.5.1"` covers both the
`0.5.1` release and every `0.5.1+<commit>` nightly. That is usually what you
want: a bug in 0.5.1 is a bug in the nightlies built on top of it. Use
`"channel": "nightly"` when a message applies *only* to people running nightly
builds.

Because every nightly stamps its own commit into the version
(`0.5.1+abc1234`, see [Versioning](../VERSIONING.md)), a user reporting a
problem is also reporting the exact commit they are on.

## Retiring a message

Set `expires` to a date, or delete the entry. Deleting is fine — nothing else
references it — but an `expires` written when the message is published means one
less thing to remember.

<div class="section_buttons">

| Previous                          |                            Next |
|:----------------------------------|--------------------------------:|
| [Configuration](configuration.md) | [App Examples](app_examples.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
