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
    "stock_common": ["ziggurat-stock-compat.patch", "ziggurat-syslog.patch"],
    "multipan_common": [
        "ziggurat-multipan-stock-compat.patch",
        "ziggurat-multipan-syslog.patch",
    ],
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
    parser.add_argument("--build-worktree", type=Path)
    parser.add_argument("--source-revision")
    parser.add_argument("--multipan", choices=("0", "1"), required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    repository = root / "ziggurat/dependencies/ziggurat"
    worktree = (root / args.build_worktree).resolve() if args.build_worktree else None
    source_revision = args.source_revision or subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD"], text=True
    ).strip()
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
                source_revision,
            ]
        )
        try:
            names = []
            if args.multipan == "1":
                names.extend(PATCHES["multipan"])
            names.extend(PATCHES["multipan_common"] if args.multipan == "1" else PATCHES["stock_common"])
            if args.multipan == "1":
                names.extend(PATCHES["multipan_after_common"])
            names.append("ziggurat-syslog-fragments.patch")
            names.append("ziggurat-async-syslog.patch")
            if args.multipan == "1":
                names.append("ziggurat-radio-trace.patch")
                names.append("ziggurat-thread-tcp.patch")
            names.append("ziggurat-key-rng-readiness.patch")
            if args.multipan == "1":
                names.append("ziggurat-radio-timing.patch")
            for name in names:
                run(["git", "-C", str(expected), "am", str(root / "ziggurat/patches" / name)])

            if worktree is None:
                print(
                    f"Ziggurat patch series applies cleanly for revision {source_revision} "
                    f"(multipan={args.multipan})"
                )
                return 0

            actual_subjects = subprocess.check_output(
                ["git", "-C", str(worktree), "log", "--reverse", "--format=%s",
                 f"{source_revision}..HEAD"],
                text=True,
            ).splitlines()
            expected_subjects = subprocess.check_output(
                ["git", "-C", str(expected), "log", "--reverse", "--format=%s",
                 f"{source_revision}..HEAD"],
                text=True,
            ).splitlines()
            if actual_subjects and actual_subjects != expected_subjects:
                raise SystemExit("Build worktree commit sequence differs from mailbox series")

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
            print(f"Ziggurat patch coverage verified for revision {source_revision}")
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
