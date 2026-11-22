#!/usr/bin/env python3
"""Build Hermes's Web UI and stage the Linux runtime assets."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def stage_assets(root: Path, output: Path) -> None:
    source = root / "src_assets"
    linux = source / "linux"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix="hermes-assets-") as tmp:
        assets = Path(tmp) / "assets"
        shutil.copytree(source / "common/assets", assets, ignore=shutil.ignore_patterns("web"))
        shutil.copytree(linux / "assets", assets, dirs_exist_ok=True)
        shutil.copytree(linux / "misc/session-compositors", assets / "session-compositors")

        env = os.environ.copy()
        env.update(HERMES_SOURCE_ASSETS_DIR=str(source), HERMES_ASSETS_DIR=tmp)
        subprocess.run(["bun", "run", "build"], cwd=root, env=env, check=True)

        if output.exists():
            shutil.rmtree(output)
        assets.rename(output)


if __name__ == "__main__":
    stage_assets(Path(sys.argv[1]).resolve(), Path(sys.argv[2]))
