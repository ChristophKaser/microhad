#!/usr/bin/env python3
"""Build an application-only radio release archive; never access a gateway."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/firmware-hdlc"
TEMPLATES = ROOT / "firmware/release"
SDK_REVISION = "853854753883af3c48271aa2060a5f8ffd8b070b"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def git(*args, cwd=ROOT):
    return subprocess.check_output(["git", "-C", str(cwd), *args], text=True).strip()


def package(version, destination, epoch, provenance, build=BUILD, arm=None):
    image = build / "build/debug/ot-rcp-multipan.gbl"
    data = image.read_bytes()
    checksum = hashlib.sha256(data).hexdigest()
    flasher = load("radio_release_flash", TEMPLATES / "flash.py")
    flasher.dependencies()
    flasher.validate_image(image, checksum)
    notices = load("radio_release_notices", ROOT / "firmware/scripts/release-notices.py")
    license_text = notices.collect(build, **({"arm": arm} if arm else {}))
    substitutions = {"@VERSION@": version, "@IMAGE_SHA256@": checksum,
                     "@PROVENANCE@": provenance}
    def render(path):
        text = path.read_text()
        for token, value in substitutions.items():
            text = text.replace(token, value)
        return text.encode()
    members = {"ot-rcp-multipan.gbl": data, "flash.py": render(TEMPLATES / "flash.py"),
               "readme.md": render(TEMPLATES / "readme.md"), "license.md": license_text.encode()}
    # Exclusive creation also protects against a racing concurrent builder.
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".radio-release-", dir=destination.parent) as staging:
        temporary = Path(staging) / destination.name
        with temporary.open("xb") as output, gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                for name, content in members.items():
                    member = tarfile.TarInfo("ot-rcp-multipan-" + version + "/" + name)
                    member.size = len(content)
                    member.mode = 0o755 if name == "flash.py" else 0o644
                    member.mtime = epoch
                    member.uid = member.gid = 0
                    archive.addfile(member, io.BytesIO(content))
        # Hardlink publication is atomic and refuses any existing destination.
        os.link(temporary, destination)
    return checksum


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", args.version) or args.jobs < 1:
        parser.error("Use a safe release version and positive --jobs")
    destination = ROOT / "build/releases/firmware" / args.version / ("ot-rcp-multipan-" + args.version + ".tar.gz")
    if destination.parent.exists():
        raise ValueError("Release output already exists: " + str(destination.parent))
    sdk = ROOT / "firmware/dependencies/gecko-sdk"
    if git("rev-parse", "HEAD", cwd=sdk) != SDK_REVISION or git("status", "--porcelain", "--untracked-files=no", cwd=sdk):
        raise ValueError("Firmware release requires the pinned, unmodified Gecko SDK")
    reference = ROOT / "firmware/dependencies/rtl8196e-gateway"
    if git("status", "--porcelain", "--untracked-files=no", cwd=reference):
        raise ValueError("Firmware reference sources contain uncommitted changes")
    flasher = load("release_preflight", TEMPLATES / "flash.py")
    flasher.dependencies()  # fail before an expensive build if the venv is missing
    revision = git("rev-parse", "HEAD")
    provenance = ("Project revision: `" + revision + "`\n\nGecko SDK: `" + SDK_REVISION +
                  "`\n\nGateway reference: `" + git("rev-parse", "HEAD", cwd=reference) + "`")
    if git("status", "--porcelain", "--untracked-files=normal", "--", "firmware", "LICENSE"):
        provenance += "\n\nBuilt with local project changes; commit release inputs before publication."
    env = dict(os.environ, BUILD_JOBS=str(args.jobs))
    subprocess.run(["bash", "firmware/scripts/build-hdlc-firmware.sh"], cwd=ROOT, env=env, check=True)
    subprocess.run(["bash", "firmware/scripts/package-hdlc-gbl.sh"], cwd=ROOT, env=env, check=True)
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", git("show", "-s", "--format=%ct", "HEAD")))
    arm = Path(env["ARM_GCC_DIR"]) if env.get("ARM_GCC_DIR") else None
    checksum = package(args.version, destination, epoch, provenance, arm=arm)
    print(str(destination))
    print("GBL SHA-256: " + checksum)
    print("Archive SHA-256: " + hashlib.sha256(destination.read_bytes()).hexdigest())
    print("Offline build only; no device accessed or release published. See THIRD_PARTY.md publication requirements.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit("Error: " + str(error))
