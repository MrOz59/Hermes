#!/usr/bin/env python3
"""Stage the native Linux package payload from Hermes mcpp build artifacts."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

FQDN = "io.github.mroz59.Hermes"
REPO = Path(__file__).resolve().parents[2]


def pkg_directory(package, variable):
    result = subprocess.run(
        ["pkg-config", f"--variable={variable}", package],
        check=True,
        capture_output=True,
        text=True,
    )
    path = Path(result.stdout.strip())
    if not path.is_relative_to("/usr") or ".." in path.parts:
        raise ValueError(f"{package}.{variable} must be a directory under /usr")
    return path.relative_to("/")


def template_values(version_header):
    # Use the metadata that actually went into this binary.
    for line in version_header.read_text(encoding="utf-8").splitlines():
        if line.startswith("#define PROJECT_VERSION "):
            version = json.loads(line.removeprefix("#define PROJECT_VERSION "))
            break
    else:
        raise ValueError(f"PROJECT_VERSION is missing from {version_header}")

    year, month, day = "1990", "01", "01"
    if release := re.fullmatch(r"(\d{4})\.(\d{3,4})\.\d+", version):
        year, month_day = release.groups()
        month, day = month_day[:-2].zfill(2), month_day[-2:]

    return {
        "CMAKE_INSTALL_FULL_DATAROOTDIR": "/usr/share",
        "PROJECT_BRIEF_DESCRIPTION": "GameStream host for Hestia",
        "PROJECT_DAY": day,
        "PROJECT_DESCRIPTION": "Self-hosted game stream host for Hestia and Artemis",
        "PROJECT_FQDN": FQDN,
        "PROJECT_HOMEPAGE_URL": "https://github.com/MrOz59/Hermes",
        "PROJECT_LICENSE": "GPL-3.0-only",
        "PROJECT_LONG_DESCRIPTION": (
            "Offering low latency, cloud gaming server capabilities with support for AMD, Intel, "
            "and Nvidia GPUs for hardware encoding. Software encoding is also available. You can "
            "connect to Hermes from any Hestia or Artemis client on a variety of devices. A web UI "
            "is provided to allow configuration, and client pairing, from your favorite web browser. "
            "Pair from the local server or any mobile device."
        ),
        "PROJECT_MONTH": month,
        "PROJECT_NAME": "Hermes",
        "PROJECT_VERSION": version,
        "PROJECT_YEAR": year,
        "SUNSHINE_DESKTOP_ICON": "hermes",
        "SUNSHINE_SERVICE_START_COMMAND": "ExecStart=hermes",
        "SUNSHINE_SERVICE_STOP_COMMAND": "",
    }


def read_results(results):
    artifacts = {}
    output = None
    for result in results:
        envelope = json.loads(result.read_text(encoding="utf-8"))
        if envelope["kind"] == "mcpp.last-result":
            result = Path(envelope["data"]["result"])
            envelope = json.loads(result.read_text(encoding="utf-8"))
        if envelope["kind"] != "mcpp.result" or envelope["data"]["status"] != "ok":
            raise ValueError(f"no successful mcpp build in {result}")
        for artifact in envelope["data"]["artifacts"]:
            if artifact["kind"] != "bin":
                continue
            name = artifact["name"]
            path = Path(artifact["path"])
            if artifacts.setdefault(name, path) != path:
                raise ValueError(f"conflicting mcpp results for {name}")
            if name == "hermes":
                resolution = json.loads(
                    result.with_name("resolution.json").read_text(encoding="utf-8")
                )
                for package in resolution["graph"]["packages"]:
                    if package["package"]["name"] == "hermes-host":
                        output = Path(package["outDir"])
                        break
    if output is None:
        raise ValueError("the mcpp results did not include hermes-host")
    return {
        "host": artifacts["hermes"],
        "assets": output / "assets",
        "version_header": output / "hermes-build-version.h",
        "card_broker": artifacts.get("hermes-kms-card-broker"),
        "session_broker": artifacts.get("hermes-session-broker"),
        "detached_session": artifacts.get("hermes-detached-session"),
        "gamemode": artifacts.get("hermes-gamemode"),
    }


def stage_pyrowave(root, args, install):
    dynamic = subprocess.check_output(["readelf", "-d", args.host], text=True)
    dependency = re.search(r"\(NEEDED\)[^\n]*\[(libpyrowave-shared\.so[^\]]*)\]", dynamic)
    if dependency is None:
        if args.pyrowave_prefix or args.libdir:
            raise ValueError("Hermes was built without shared PyroWave")
        return

    if not args.pyrowave_prefix or not args.libdir:
        raise ValueError("pass --pyrowave-prefix and --libdir to stage the linked PyroWave runtime")
    if not args.libdir.parts or args.libdir.is_absolute() or ".." in args.libdir.parts:
        raise ValueError("--libdir must be relative to /usr")
    relative = Path("usr") / args.libdir / "hermes"
    runpath = re.search(r"\(RUNPATH\)[^\n]*\[([^\]]*)\]", dynamic)
    rpath = re.search(r"\(RPATH\)[^\n]*\[([^\]]*)\]", dynamic)
    expected = f"/{relative}"
    if runpath is None and rpath and rpath[1] == expected:
        # mcpp 2026.11 forces legacy ELF dtags. Convert the staged copy to
        # the RUNPATH used by native packages, leaving the build artifact alone.
        patchelf = REPO / ".toolchain/patchelf-0.18.0/bin/patchelf"
        staged_host = root / "usr/bin/hermes"
        subprocess.run([str(patchelf) if patchelf.is_file() else "patchelf",
                        "--set-rpath", expected, staged_host], check=True)
        patched = subprocess.check_output(["readelf", "-d", staged_host], text=True)
        runpath = re.search(r"\(RUNPATH\)[^\n]*\[([^\]]*)\]", patched)
    if runpath is None or runpath[1] != expected:
        raise ValueError(f"Hermes must have install RUNPATH {expected}")

    source = args.pyrowave_prefix / "lib"
    if not (source / dependency[1]).is_file():
        raise ValueError(f"PyroWave SDK lacks {dependency[1]}")
    destination = root / relative
    destination.mkdir(parents=True, exist_ok=True)
    for path in source.glob("libpyrowave-shared.so*"):
        if path.is_symlink():
            link = path.readlink()
            if link.name != str(link) or not (source / link).is_file():
                raise ValueError(f"invalid PyroWave library symlink: {path}")
            (destination / path.name).symlink_to(link)
        else:
            install(path, destination / path.name)
    licenses = args.pyrowave_prefix / "share/licenses/pyrowave"
    if licenses.is_dir():
        for path in licenses.iterdir():
            install(path, root / "usr/share/licenses/hermes/pyrowave" / path.name)


def stage_payload(root, args, substitutions):
    binaries = root / "usr/bin"
    share = root / "usr/share"
    assets = share / "hermes"
    misc = REPO / "src_assets/linux/misc"
    templates = REPO / "packaging/linux"
    icons = share / "icons/hicolor/scalable"
    udev = root / pkg_directory("udev", "udev_dir") / "rules.d"
    user_units = root / pkg_directory("systemd", "systemd_user_unit_dir")
    system_units = root / pkg_directory("systemd", "systemd_system_unit_dir")
    sysusers = root / pkg_directory("systemd", "sysusers_dir")
    modules = root / pkg_directory("systemd", "modules_load_dir")

    def install(source, destination, mode=0o644):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(mode)

    def render(source, destination):
        text = (templates / source).read_text(encoding="utf-8")
        text = re.sub(r"@([A-Za-z_][A-Za-z0-9_]*)@", lambda match: substitutions[match[1]], text)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text, encoding="utf-8")
        destination.chmod(0o644)

    shutil.copytree(args.assets, assets)
    for path in assets.rglob("*"):
        if path.is_file():
            path.chmod(0o644)

    install(args.host, binaries / "hermes", mode=0o755)
    for name in ("hermes-gamescope-launch", "hermes-monitor-recovery"):
        install(misc / name, binaries / name, mode=0o755)
    for binary, name, allow in (
        (args.card_broker, "hermes-kms-card-broker", "card-broker"),
        (args.session_broker, "hermes-session-broker", "session-broker"),
    ):
        if binary:
            install(binary, binaries / name, mode=0o755)
            install(misc / f"{allow}.allow", assets / f"{allow}.allow.example")
            for extension in ("socket", "service"):
                unit = f"{name}.{extension}"
                install(misc / unit, system_units / unit)

    if args.detached_session:
        install(args.detached_session, binaries / "hermes-detached-session", mode=0o755)
    if args.gamemode:
        install(args.gamemode, binaries / "hermes-gamemode", mode=0o755)
        desktop = f"{FQDN}.GameMode.desktop"
        render(desktop, share / "applications" / desktop)
    if args.tray:
        install(REPO / "hermes.svg", icons / "status/hermes-tray.svg")
        tray_images = REPO / "src_assets/common/assets/web/public/images"
        for name in ("hermes-playing.svg", "hermes-pausing.svg", "hermes-locked.svg"):
            install(tray_images / name, icons / "status" / name)

    for name in ("60-hermes.rules", "74-hermes-session-acl.rules", "99-hermes-session-input.rules"):
        install(misc / name, udev / name)
    install(misc / "10-hermes-session-deny.rules", share / "polkit-1/rules.d/10-hermes-session-deny.rules")
    install(misc / "hermes-sysusers.conf", sysusers / "hermes.conf")
    for name in ("60-hermes.conf", "61-hermes-evdi.conf"):
        install(misc / name, modules / name)
    install(misc / "hermes-evdi-modprobe.conf", modules.parent / "modprobe.d/hermes-evdi.conf")
    install(REPO / "hermes.svg", icons / "apps/hermes.svg")
    for desktop in (f"{FQDN}.desktop", f"{FQDN}.terminal.desktop"):
        render(desktop, share / "applications" / desktop)
    render(f"{FQDN}.metainfo.xml", share / "metainfo" / f"{FQDN}.metainfo.xml")
    render("sunshine.service.in", user_units / "hermes.service")
    stage_pyrowave(root, args, install)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="a new staging directory")
    parser.add_argument("--result", action="append", type=Path,
                        help="mcpp result.json or .last-result.json (repeatable)")
    parser.add_argument("--host", type=Path)
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--version-header", type=Path)
    parser.add_argument("--card-broker", type=Path)
    parser.add_argument("--session-broker", type=Path)
    parser.add_argument("--detached-session", type=Path)
    parser.add_argument("--gamemode", type=Path)
    parser.add_argument("--tray", action="store_true")
    parser.add_argument("--pyrowave-prefix", type=Path)
    parser.add_argument("--libdir", type=Path)
    args = parser.parse_args()

    try:
        if args.result:
            for name, path in read_results(args.result).items():
                if getattr(args, name) is not None:
                    raise ValueError("--result cannot be combined with explicit artifact paths")
                setattr(args, name, path)
        if not (args.host and args.assets and args.version_header):
            raise ValueError("pass --result or supply --host, --assets and --version-header")
        if b"/usr/share/hermes\0" not in args.host.read_bytes():
            raise ValueError("build hermes-host with the package-assets feature")
        if args.root.exists() or args.root.is_symlink():
            raise ValueError(f"staging directory already exists: {args.root}")
        for relative in ("apps.json", "web/index.html", "session-compositors/weston.conf"):
            if not (args.assets / relative).is_file():
                raise ValueError(f"missing runtime asset {relative} in {args.assets}")
        substitutions = template_values(args.version_header)
        with tempfile.TemporaryDirectory(dir=args.root.parent, prefix="hermes-stage-") as work:
            root = Path(work) / "payload"
            stage_payload(root, args, substitutions)
            for directory in root.rglob("*"):
                if directory.is_dir():
                    directory.chmod(0o755)
            root.chmod(0o755)
            root.rename(args.root)
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"stage-install: {error}\n")


if __name__ == "__main__":
    main()
