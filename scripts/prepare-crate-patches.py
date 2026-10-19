#!/usr/bin/env python3
"""Prepare isolated Cargo source trees and apply their mailbox commits."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import release_source


ROOT = Path(__file__).resolve().parent.parent
REGISTRY = Path(os.environ["CARGO_HOME"]) / "registry" / "src"


def run(command: list[str], *, cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    destination_root = ROOT / "build/patched-crates-mailbox"
    destination_root.mkdir(parents=True, exist_ok=True)
    for crate in sys.argv[1:]:
        if release_source.enabled():
            release_source.verify('build/patched-crates-mailbox/' + crate)
            continue
        sources = list(REGISTRY.glob(f"*/{crate}"))
        if len(sources) != 1:
            raise SystemExit(f"Expected one fetched Cargo source for {crate}, found {len(sources)}")

        destination = destination_root / crate
        if not destination.exists():
            shutil.copytree(sources[0], destination, symlinks=True)
            run(["git", "init", "--quiet", "-b", "mailbox-base"], cwd=destination)
            run(["git", "config", "user.name", "Pinned Cargo source"], cwd=destination)
            run(["git", "config", "user.email", "cargo-source@localhost"], cwd=destination)
            run(["git", "add", "-A"], cwd=destination)
            run(["git", "commit", "--quiet", "-m", f"Import crates.io {crate} source"], cwd=destination)
            run(["git", "tag", "mailbox-base"], cwd=destination)

        base = subprocess.check_output(
            ["git", "-C", str(destination), "rev-parse", "refs/tags/mailbox-base"], text=True
        ).strip()
        patch = ROOT / "patches" / f"{crate}.patch"
        run([
            "python3", str(ROOT / "scripts/apply-mailbox-series.py"),
            str(destination), base, str(patch),
        ])
        print(f"Prepared {crate} from mailbox commit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
