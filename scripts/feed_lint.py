#!/usr/bin/env python3
"""Validate a Hermes announcement feed document before it is published.

The Web UI silently drops any entry it cannot make sense of, so a typo in
feed.json shows up as a missing announcement rather than an error. This script
applies the same rules the client does (see
src_assets/common/assets/web/feed.js) and reports what would be dropped.

Usage:
    scripts/feed_lint.py feed.json
"""

import argparse
import datetime
import json
import re
import sys
from urllib.parse import urlparse

LEVELS = ("danger", "warning", "info", "success")
CHANNELS = ("all", "stable", "nightly")
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
CLAUSE_PATTERN = re.compile(r"^(>=|<=|>|<|=)?v?\d+\.\d+\.\d+$")
MESSAGE_KEYS = {
    "id", "title", "body", "level", "channel", "affects",
    "link", "starts", "expires", "dismissible", "revision",
}
DOCUMENT_KEYS = {"$schema", "version", "messages"}


def check_timestamp(errors, where, field, value):
    """Accept what Date.parse() accepts in practice: an ISO date or timestamp."""
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{where}: {field} must be an ISO 8601 date string")
        return
    text = value.strip().replace("Z", "+00:00")
    try:
        datetime.datetime.fromisoformat(text)
    except ValueError:
        errors.append(f"{where}: {field} is not an ISO 8601 date ({value!r})")


def check_link(errors, where, value):
    url = value if isinstance(value, str) else None
    if isinstance(value, dict):
        if set(value) - {"url", "label"}:
            errors.append(f"{where}: link accepts only 'url' and 'label'")
        url = value.get("url")
        label = value.get("label")
        if label is not None and not isinstance(label, str):
            errors.append(f"{where}: link.label must be a string")
    if not isinstance(url, str):
        errors.append(f"{where}: link must be a URL string or an object with a 'url'")
        return
    parsed = urlparse(url.strip())
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        errors.append(f"{where}: link must be an absolute http(s) URL, not {url!r} (the client discards it)")


def check_affects(errors, where, value):
    ranges = value if isinstance(value, list) else [value]
    if not ranges:
        errors.append(f"{where}: affects must not be an empty list")
    for entry in ranges:
        if not isinstance(entry, str):
            errors.append(f"{where}: each affects range must be a string")
            continue
        if entry.strip() == "*":
            continue
        clauses = entry.split()
        if not clauses:
            errors.append(f"{where}: affects range is empty")
        for clause in clauses:
            if not CLAUSE_PATTERN.match(clause):
                errors.append(
                    f"{where}: {clause!r} is not a valid range clause "
                    f"(expected something like '>=0.5.0', '<0.5.2' or '0.5.1')"
                )


def check_message(errors, index, seen_ids, message):
    where = f"messages[{index}]"
    if not isinstance(message, dict):
        errors.append(f"{where}: must be an object")
        return

    unknown = set(message) - MESSAGE_KEYS
    if unknown:
        errors.append(f"{where}: unknown field(s) {', '.join(sorted(unknown))} (the client ignores them)")

    identifier = message.get("id")
    if not isinstance(identifier, str) or not identifier.strip():
        errors.append(f"{where}: id is required and must be a non-empty string")
    else:
        identifier = identifier.strip()
        where = f"messages[{index}] ({identifier})"
        if not ID_PATTERN.match(identifier):
            errors.append(f"{where}: id must be a lowercase slug, e.g. 'kms-cursor-rdna3'")
        if identifier in seen_ids:
            errors.append(f"{where}: duplicate id — only the first entry is shown")
        seen_ids.add(identifier)

    title = message.get("title")
    if not isinstance(title, str) or not title.strip():
        errors.append(f"{where}: title is required and must be a non-empty string")

    if "body" in message and not isinstance(message["body"], str):
        errors.append(f"{where}: body must be a string")
    if "level" in message and message["level"] not in LEVELS:
        errors.append(f"{where}: level must be one of {', '.join(LEVELS)}")
    if "channel" in message and message["channel"] not in CHANNELS:
        errors.append(f"{where}: channel must be one of {', '.join(CHANNELS)}")
    if "dismissible" in message and not isinstance(message["dismissible"], bool):
        errors.append(f"{where}: dismissible must be true or false")
    if "revision" in message:
        revision = message["revision"]
        if not isinstance(revision, int) or isinstance(revision, bool) or revision < 1:
            errors.append(f"{where}: revision must be an integer >= 1")
    if "affects" in message:
        check_affects(errors, where, message["affects"])
    if "link" in message:
        check_link(errors, where, message["link"])
    for field in ("starts", "expires"):
        if field in message:
            check_timestamp(errors, where, field, message[field])

    starts, expires = message.get("starts"), message.get("expires")
    if isinstance(starts, str) and isinstance(expires, str) and starts.strip() >= expires.strip():
        errors.append(f"{where}: expires is not after starts, so the message would never appear")


def lint(document):
    errors = []
    if isinstance(document, list):
        messages = document
    elif isinstance(document, dict):
        unknown = set(document) - DOCUMENT_KEYS
        if unknown:
            errors.append(f"document: unknown field(s) {', '.join(sorted(unknown))}")
        messages = document.get("messages")
        if not isinstance(messages, list):
            errors.append("document: 'messages' is required and must be a list")
            return errors
    else:
        return ["document: the feed must be an object with a 'messages' list"]

    seen_ids = set()
    for index, message in enumerate(messages):
        check_message(errors, index, seen_ids, message)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", help="path to the feed document")
    args = parser.parse_args()

    try:
        with open(args.path, encoding="utf-8") as handle:
            document = json.load(handle)
    except OSError as error:
        print(f"cannot read {args.path}: {error}", file=sys.stderr)
        return 1
    except json.JSONDecodeError as error:
        print(f"{args.path} is not valid JSON: {error}", file=sys.stderr)
        return 1

    errors = lint(document)
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"\n{len(errors)} problem(s) found in {args.path}", file=sys.stderr)
        return 1

    count = len(document if isinstance(document, list) else document.get("messages", []))
    print(f"{args.path}: OK ({count} message(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
