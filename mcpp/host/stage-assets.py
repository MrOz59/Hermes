#!/usr/bin/env python3
"""Build Hermes's Web UI and stage the Linux runtime assets."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def asset_inputs(source: Path):
    common = source / "common/assets"
    web = common / "web"
    for tree in (common, source / "linux/assets", source / "linux/misc/session-compositors"):
        for name, directories, files in os.walk(tree):
            directory = Path(name)
            if directory == web:
                # Bun's installed packages and the test suite are not web build inputs.
                directories[:] = [child for child in directories if child not in {"node_modules", "tests"}]
            # Watching directory mtimes also detects newly added or renamed files.
            yield directory
            yield from (directory / file for file in files)


def depfile_word(path: Path) -> str:
    return str(path).replace("\\", "\\\\").replace(" ", "\\ ").replace("#", "\\#").replace("$", "$$")


def stage_assets(root: Path, output: Path, stamp: Path, depfile: Path) -> None:
    source = root / "src_assets"
    web = source / "common/assets/web"
    linux = source / "linux"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix="hermes-assets-") as tmp:
        assets = Path(tmp) / "assets"
        shutil.copytree(source / "common/assets", assets, ignore=shutil.ignore_patterns("web"))
        shutil.copytree(linux / "assets", assets, dirs_exist_ok=True)
        shutil.copytree(linux / "misc/session-compositors", assets / "session-compositors")

        env = os.environ.copy()
        env["HERMES_ASSETS_DIR"] = tmp
        subprocess.run(["bun", "run", "build"], cwd=web, env=env, check=True)

        if output.exists():
            shutil.rmtree(output)
        assets.rename(output)

    inputs = " ".join(depfile_word(path) for path in sorted(asset_inputs(source)))
    depfile.write_text(f"{depfile_word(stamp)}: {inputs}\n", encoding="utf-8")


if __name__ == "__main__":
    root, output, stamp, depfile = map(Path, sys.argv[1:])
    stage_assets(root.resolve(), output, stamp, depfile)
