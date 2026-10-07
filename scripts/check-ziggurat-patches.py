#!/usr/bin/env python3
"""Ensure the pinned Ziggurat build worktree is fully represented by patches/."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


PATCHES = {
    "multipan": [
        "ziggurat-hdlc-multipan.patch",
        "ziggurat-network-migration.patch",
    ],
    "common": ["ziggurat-stock-compat.patch", "ziggurat-syslog.patch"],
    "multipan_after_common": [
        "ziggurat-cobs-unix.patch",
        "ziggurat-poll-diagnostics.patch",
        "ziggurat-parent-announcement.patch",
        "ziggurat-fresh-join-replay.patch",
    ],
}


def run(command: list[str], *, cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def tree_files(root: Path) -> dict[str, tuple[bytes, int] | str]:
    files: dict[str, tuple[bytes, int] | str] = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if relative.parts[0] == ".git" or relative == Path("Cargo.lock"):
            continue
        name = relative.as_posix()
        if path.is_symlink():
            files[name] = f"symlink:{os.readlink(path)}"
        elif path.is_file():
            files[name] = (path.read_bytes(), path.stat().st_mode & 0o777)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_worktree", type=Path)
    parser.add_argument("source_revision")
    parser.add_argument("multipan", choices=("0", "1"))
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    repository = root / "ziggurat"
    worktree = (root / args.build_worktree).resolve()
    actual_revision = subprocess.check_output(
        ["git", "-C", str(worktree), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual_revision != args.source_revision:
        raise SystemExit(
            f"Build worktree HEAD {actual_revision} differs from pinned revision "
            f"{args.source_revision}"
        )

    with tempfile.TemporaryDirectory(prefix="ziggurat-patch-check-") as temporary:
        expected = Path(temporary) / "source"
        run(
            [
                "git",
                "-C",
                str(repository),
                "worktree",
                "add",
                "--detach",
                str(expected),
                args.source_revision,
            ]
        )
        try:
            names = []
            if args.multipan == "1":
                names.extend(PATCHES["multipan"])
            names.extend(PATCHES["common"])
            if args.multipan == "1":
                names.extend(PATCHES["multipan_after_common"])
            for name in names:
                run(["git", "-C", str(expected), "apply", str(root / "patches" / name)])

            expected_files = tree_files(expected)
            actual_files = tree_files(worktree)
            expected_names = set(expected_files)
            actual_names = set(actual_files)
            problems = []
            problems.extend(
                f"unpatched build file: {name}"
                for name in sorted(actual_names - expected_names)
            )
            problems.extend(
                f"missing patched file: {name}"
                for name in sorted(expected_names - actual_names)
            )
            problems.extend(
                f"content differs from patch set: {name}"
                for name in sorted(expected_names & actual_names)
                if expected_files[name] != actual_files[name]
            )
            if problems:
                raise SystemExit("Ziggurat patch coverage failed:\n  " + "\n  ".join(problems))
            print(f"Ziggurat patch coverage verified for revision {args.source_revision}")
        finally:
            subprocess.run(
                ["git", "-C", str(repository), "worktree", "remove", "--force", str(expected)],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
