#!/usr/bin/env python3
"""Exercise the installed launcher contract without starting a compositor/Steam."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

LAUNCHER = Path(__file__).resolve().parents[2] / "src_assets/linux/misc/hermes-gamescope-launch"


class GamescopeLaunch(unittest.TestCase):
    def launch(self, **overrides):
        with tempfile.TemporaryDirectory(prefix="hermes-gamescope-") as temp:
            root = Path(temp)
            for name in ("gamescope", "steam"):
                binary = root / name
                binary.write_text(
                    "#!/usr/bin/python3\nimport json, os, sys\n"
                    "from pathlib import Path\n"
                    "Path(os.environ['RESULT']).write_text(json.dumps({"
                    "'argv': sys.argv, 'dxvk': os.environ.get('DXVK_HDR'), "
                    "'proton': os.environ.get('PROTON_ENABLE_HDR')}))\n"
                )
                binary.chmod(0o755)
            env = {"PATH": f"{root}:/usr/bin:/bin", "HOME": temp,
                   "XDG_STATE_HOME": temp, "RESULT": str(root / "result"),
                   "DXVK_HDR": "0", "PROTON_ENABLE_HDR": "0"}
            env.update(overrides)
            result = subprocess.run(["bash", str(LAUNCHER)], env=env, capture_output=True, timeout=5)
            payload = json.loads((root / "result").read_text()) if (root / "result").exists() else None
            return result.returncode, payload

    def test_hdr_aliases_and_argument_boundary(self):
        for alias in ("HERMES_CLIENT_HDR", "APOLLO_CLIENT_HDR", "SUNSHINE_CLIENT_HDR"):
            with self.subTest(alias=alias):
                code, result = self.launch(**{alias: "true"})
                self.assertEqual(code, 0)
                args = result["argv"]
                self.assertIn("--backend=wayland", args)
                self.assertLess(args.index("--hdr-enabled"), args.index("--"))
                self.assertFalse(any("debug-force" in arg for arg in args))
                self.assertEqual((result["dxvk"], result["proton"]), ("1", "1"))

    def test_sdr_wins_over_legacy_hdr_and_inherited_overrides(self):
        code, result = self.launch(HERMES_CLIENT_HDR="false", APOLLO_CLIENT_HDR="true",
                                   DXVK_HDR="1", PROTON_ENABLE_HDR="1")
        self.assertEqual(code, 0)
        self.assertNotIn("--hdr-enabled", result["argv"])
        self.assertEqual((result["dxvk"], result["proton"]), ("0", "0"))

    def test_unsupported_hdr_backend_never_launches(self):
        self.assertEqual(self.launch(HERMES_CLIENT_HDR="true", HERMES_GAMESCOPE_BACKEND="sdl"), (2, None))

    def test_sdl_sdr_still_launches(self):
        code, result = self.launch(HERMES_GAMESCOPE_BACKEND="sdl")
        self.assertEqual(code, 0)
        self.assertIn("--backend=sdl", result["argv"])

    def test_existing_session_does_not_nest(self):
        code, result = self.launch(HERMES_CLIENT_HDR="1", GAMESCOPE_WAYLAND_DISPLAY="gamescope-0")
        self.assertEqual(code, 0)
        self.assertEqual(Path(result["argv"][0]).name, "steam")
        self.assertEqual(result["argv"][1:], ["-bigpicture"])
        self.assertEqual(result["proton"], "1")

    def test_invalid_hdr_never_launches(self):
        self.assertEqual(self.launch(HERMES_CLIENT_HDR="typo"), (2, None))


if __name__ == "__main__":
    unittest.main()
