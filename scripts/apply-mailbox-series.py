#!/usr/bin/env python3
"""Apply an ordered mailbox series in a pinned-source build worktree."""

from __future__ import annotations

import argparse
from email.parser import Parser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def git(repo: Path, *args: str, capture: bool = False, check: bool = True) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout if capture else ""


def files(root: Path) -> dict[str, tuple[bytes, int] | str]:
    result: dict[str, tuple[bytes, int] | str] = {}
    # Gitlinks pin nested checkouts; their files are validated by preparation.
    entries = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "--stage"], text=True
    ).splitlines()
    gitlinks = [Path(line.split("\t", 1)[1]) for line in entries if line.startswith("160000 ")]
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if ".git" in relative.parts or relative == Path("Cargo.lock") or any(
            relative == link or link in relative.parents for link in gitlinks
        ):
            continue
        if path.is_symlink():
            result[relative.as_posix()] = f"symlink:{os.readlink(path)}"
        elif path.is_file():
            # Compare Git-relevant executable state, not incidental group-write
            # permissions from the checkout's umask.
            result[relative.as_posix()] = (path.read_bytes(), path.stat().st_mode & 0o111)
    return result


def mailbox_subject(path: Path) -> str:
    message = Parser().parsestr(path.read_text())
    subject = message.get("Subject", "")
    if subject.startswith("[PATCH]"):
        subject = subject[len("[PATCH]"):].strip()
    elif subject.startswith("[PATCH ") and "]" in subject:
        subject = subject.split("]", 1)[1].strip()
    if not subject:
        raise SystemExit(f"Mailbox has no Subject header: {path}")
    return subject


def status_without_lock(repo: Path) -> list[str]:
    lines = git(repo, "status", "--porcelain", "--untracked-files=all", capture=True).splitlines()
    return [line for line in lines if line[3:] != "Cargo.lock"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path, help="existing linked build worktree")
    parser.add_argument("base_revision")
    parser.add_argument("mailboxes", nargs="+", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    base = args.base_revision
    mailboxes = [path.resolve() for path in args.mailboxes]
    expected_subjects = [mailbox_subject(path) for path in mailboxes]

    actual_head = git(repo, "rev-parse", "HEAD", capture=True).strip()
    ancestor = subprocess.run(
        ["git", "-C", str(repo), "merge-base", "--is-ancestor", base, actual_head],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0
    if not ancestor:
        raise SystemExit(f"Worktree HEAD {actual_head} is not based on pinned revision {base}")

    actual_subjects = git(
        repo, "log", "--reverse", "--format=%s", f"{base}..HEAD", capture=True
    ).splitlines()
    if actual_subjects != expected_subjects[:len(actual_subjects)]:
        raise SystemExit("Worktree contains commits outside the expected mailbox series")

    # Build expected trees from the mailbox commits. This also safely recognizes and
    # migrates a legacy git-apply worktree, but only when its contents match exactly.
    scratch = Path(tempfile.mkdtemp(prefix="mailbox-series-check-", dir=ROOT / "build"))
    expected = scratch / "expected"
    git(repo, "worktree", "add", "--quiet", "--detach", str(expected), base)
    prefix_trees: list[dict[str, tuple[bytes, int] | str]] = [files(expected)]
    try:
        for mailbox in mailboxes:
            git(expected, "am", "--quiet", str(mailbox))
            prefix_trees.append(files(expected))

        count = len(actual_subjects)
        current_files = files(repo)
        current_status = status_without_lock(repo)
        if actual_head == base and current_status:
            # Old builds applied raw diffs. They may be replaced only if every file
            # exactly matches the final mailbox series (excluding Cargo's generated lock).
            if current_files != prefix_trees[-1]:
                raise SystemExit("Legacy patched worktree differs from mailbox series; preserving it")
            lock_path = repo / "Cargo.lock"
            lock_bytes = lock_path.read_bytes() if lock_path.exists() else None
            git(repo, "reset", "--hard", base)
            git(repo, "clean", "-fd")
            count = 0
        elif current_status:
            raise SystemExit("Worktree has uncommitted changes outside Cargo.lock; preserving it")
        elif current_files != prefix_trees[count]:
            raise SystemExit("Worktree tree does not match its applied mailbox commits")
        else:
            lock_path = repo / "Cargo.lock"
            lock_bytes = None

        if count < len(mailboxes):
            # Cargo may have rewritten this generated lockfile after the previous build.
            lock_path = repo / "Cargo.lock"
            if lock_bytes is None and lock_path.exists():
                lock_bytes = lock_path.read_bytes()
            if lock_path.exists():
                lock_path.unlink()
            for mailbox in mailboxes[count:]:
                git(repo, "am", "--quiet", str(mailbox))
            if lock_bytes is not None:
                lock_path.write_bytes(lock_bytes)

        if files(repo) != prefix_trees[-1]:
            raise SystemExit("Applied mailbox tree differs from the validated patch series")
        if status_without_lock(repo):
            raise SystemExit("Mailbox application left unexpected worktree changes")
        print(f"Applied {len(mailboxes)} mailbox commits at {git(repo, 'rev-parse', 'HEAD', capture=True).strip()}")
    finally:
        subprocess.run(
            ["git", "-C", str(repo), "worktree", "remove", "--force", str(expected)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        shutil.rmtree(scratch, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
