"""Generate the version macros consumed by Hermes's C++ sources."""

import json
import os
from pathlib import Path
import re
import subprocess
import sys


def compute_metadata(root):
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    commit = os.environ.get("COMMIT", "")
    dependencies = {root / "VERSION"}
    if os.environ.get("HERMES_BUILD_HAS_BRANCH") and os.environ.get("HERMES_BUILD_HAS_BUILD_VERSION"):
        if ci_version := os.environ.get("BUILD_VERSION"):
            version = ci_version.removeprefix("v")
    else:
        def git(*args):
            return subprocess.run(
                ["git", "-C", str(root), *args], capture_output=True,
                env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"}, check=False,
            )

        try:
            branch = git("rev-parse", "--abbrev-ref", "HEAD")
        except FileNotFoundError:
            branch = None
        if branch is not None:
            head = git("rev-parse", "HEAD")
            commit = head.stdout.decode().strip() if head.returncode == 0 else ""
            if branch.returncode == 0:
                branch_name = branch.stdout.decode().strip()
                if branch_name not in {"main", "master"}:
                    version += f".{git('rev-parse', '--short', 'HEAD').stdout.decode().strip()}"
                if git("diff", "-b", "--quiet", "--exit-code").returncode:
                    version += ".dirty"

                files = git("ls-files", "-z").stdout.split(b"\0")
                dependencies.update(root / os.fsdecode(path) for path in files if path)
                ref = git("symbolic-ref", "-q", "HEAD").stdout.decode().strip()
                for name in filter(None, ("HEAD", "index", "packed-refs", ref)):
                    path = git("rev-parse", "--git-path", name).stdout.decode().strip()
                    if path:
                        dependencies.add(root / path)

    match = re.search(r"(\d+)\.(\d+)\.(\d+)", version)
    if match is None:
        raise ValueError(f"invalid Hermes version {version!r}")
    major, minor, patch = match.groups()
    return {
        "PROJECT_NAME": "Hermes",
        "PROJECT_VERSION": version,
        "PROJECT_VERSION_MAJOR": major,
        "PROJECT_VERSION_MINOR": minor,
        "PROJECT_VERSION_PATCH": patch,
        "PROJECT_VERSION_COMMIT": commit or "unknown",
        "HERMES_PUBLISHER_NAME": "Hermes contributors",
        "HERMES_PUBLISHER_WEBSITE": "https://github.com/MrOz59/Hermes",
        "HERMES_PUBLISHER_ISSUE_URL": "https://github.com/MrOz59/Hermes/issues",
    }, {path for path in dependencies if path.is_file()}


def depfile_word(value):
    return str(value).replace("\\", "\\\\").replace(" ", "\\ ").replace("#", "\\#").replace("$", "$$")


def main():
    root, header, depfile = map(Path, sys.argv[1:])
    values, dependencies = compute_metadata(root)
    content = "#pragma once\n" + "".join(
        f"#define {macro} {json.dumps(value)}\n" for macro, value in values.items()
    )
    if not header.exists() or header.read_text(encoding="utf-8") != content:
        header.write_text(content, encoding="utf-8")
    inputs = " ".join(depfile_word(path) for path in sorted(dependencies))
    depfile.write_text(f"{depfile_word(header)}: {inputs}\n", encoding="utf-8")


if __name__ == "__main__":
    main()
