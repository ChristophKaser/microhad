#!/usr/bin/env python3
"""Validate every tracked patch and active source/patch combination offline."""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PATCH_DIRS = [ROOT / "patches", ROOT / "ziggurat/patches", ROOT / "micro-thread-matter-daemon/patches"]

def patch_path(name: str) -> Path:
    directory = "ziggurat/patches" if name.startswith("ziggurat-") else (
        "micro-thread-matter-daemon/patches" if name.startswith(("rs-matter-", "openthread-")) else "patches")
    return ROOT / directory / name

ZIGGURAT = ROOT / "ziggurat/dependencies/ziggurat"
RS_MATTER = ROOT / "micro-thread-matter-daemon/dependencies/rs-matter"

ZIGGURAT_COMMON = ["ziggurat-stock-compat.patch", "ziggurat-syslog.patch", "ziggurat-syslog-fragments.patch", "ziggurat-async-syslog.patch", "ziggurat-key-rng-readiness.patch", "ziggurat-lean-gateway.patch"]
ZIGGURAT_MULTI_COMMON = [
    "ziggurat-multipan-stock-compat.patch",
    "ziggurat-multipan-syslog.patch",
]
ZIGGURAT_MULTI = [
    "ziggurat-hdlc-multipan.patch",
    "ziggurat-network-migration.patch",
    *ZIGGURAT_MULTI_COMMON,
    "ziggurat-cobs-unix.patch",
    "ziggurat-poll-diagnostics.patch",
    "ziggurat-parent-announcement.patch",
    "ziggurat-fresh-join-replay.patch",
    "ziggurat-syslog-fragments.patch",
    "ziggurat-async-syslog.patch",
    "ziggurat-radio-trace.patch",
    "ziggurat-thread-tcp.patch",
    "ziggurat-key-rng-readiness.patch",
    "ziggurat-radio-timing.patch",
    "ziggurat-lean-gateway.patch",
]
RS_MATTER_SERVICE = [
    "rs-matter-spake2p.patch",
    "rs-matter-peer-identity.patch",
    "rs-matter-attribute-notify.patch",
    "rs-matter-icd-capture.patch",
    "rs-matter-sessionless-icd.patch",
    "rs-matter-commissioning-region.patch",
    "rs-matter-commissioning-progress.patch",
    "rs-matter-btp-response-timeout.patch",
]
CRATE_PATCHES = ["getrandom-0.4.3", "nix-0.26.4", "radium-0.7.0"]
def run(command: list[str], *, cwd: Path | None = None, capture: bool = False) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout if capture else ""


def apply_series(repo: Path, patches: list[Path]) -> None:
    for patch in patches:
        run(["git", "-C", str(repo), "am", "--quiet", str(patch)])


def temporary_worktree(source: Path, revision: str, path: Path) -> None:
    run(["git", "-C", str(source), "worktree", "add", "--quiet", "--detach", str(path), revision])


def remove_worktree(source: Path, path: Path) -> None:
    subprocess.run(
        ["git", "-C", str(source), "worktree", "remove", "--force", str(path)],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def verify_git_series(source: Path, patches: list[Path], label: str) -> None:
    revision = run(["git", "-C", str(source), "rev-parse", "HEAD"], capture=True).strip()
    with tempfile.TemporaryDirectory(prefix="patch-series-check-") as temporary:
        worktree = Path(temporary) / "source"
        temporary_worktree(source, revision, worktree)
        try:
            apply_series(worktree, patches)
        finally:
            remove_worktree(source, worktree)
    print(f"PASS {label} ({len(patches)} patches)")


def snapshot_repository(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination, symlinks=True)
    run(["git", "init", "--quiet", "-b", "mailbox-base"], cwd=destination)
    run(["git", "-c", "user.name=Mailbox Validation", "-c", "user.email=patches@localhost",
         "add", "-A"], cwd=destination)
    run(["git", "-c", "user.name=Mailbox Validation", "-c", "user.email=patches@localhost",
         "commit", "--quiet", "-m", "Import pinned source snapshot"], cwd=destination)


def verify_snapshot_series(source: Path, patches: list[Path], label: str) -> None:
    with tempfile.TemporaryDirectory(prefix="patch-snapshot-check-", dir=ROOT / "build") as temporary:
        repository = Path(temporary) / "source"
        snapshot_repository(source, repository)
        apply_series(repository, patches)
    print(f"PASS {label} ({len(patches)} mailboxes)")


def check_snapshot_worktree_matches(
    source: Path, worktree: Path, patches: list[Path], label: str
) -> None:
    with tempfile.TemporaryDirectory(prefix="patch-snapshot-coverage-", dir=ROOT / "build") as temporary:
        expected = Path(temporary) / "source"
        snapshot_repository(source, expected)
        apply_series(expected, patches)
        if tree_files(expected) != tree_files(worktree):
            raise SystemExit(f"{label} differs from its mailbox series")
    print(f"PASS {label} matches its mailbox series")


def check_worktree_matches(source: Path, worktree: Path, patches: list[Path], label: str) -> None:
    if not worktree.is_dir():
        return
    revision = run(["git", "-C", str(source), "rev-parse", "HEAD"], capture=True).strip()
    with tempfile.TemporaryDirectory(prefix="patch-worktree-coverage-") as temporary:
        expected = Path(temporary) / "source"
        temporary_worktree(source, revision, expected)
        try:
            apply_series(expected, patches)
            expected_files = tree_files(expected)
            actual_files = tree_files(worktree)
            expected_subjects = run(
                ["git", "-C", str(expected), "log", "--reverse", "--format=%s", f"{revision}..HEAD"],
                capture=True,
            ).splitlines()
        finally:
            remove_worktree(source, expected)
    actual_subjects = run(
        ["git", "-C", str(worktree), "log", "--reverse", "--format=%s", f"{revision}..HEAD"],
        capture=True,
    ).splitlines()
    if actual_subjects and actual_subjects != expected_subjects:
        raise SystemExit(f"{label} build worktree commit sequence differs from its mailboxes")
    if expected_files != actual_files:
        expected_names = set(expected_files)
        actual_names = set(actual_files)
        problems = [f"unpatched build file: {name}" for name in sorted(actual_names - expected_names)]
        problems.extend(f"missing patched file: {name}" for name in sorted(expected_names - actual_names))
        problems.extend(
            f"content differs from patch set: {name}"
            for name in sorted(expected_names & actual_names)
            if expected_files[name] != actual_files[name]
        )
        raise SystemExit(f"{label} patch coverage failed:\n  " + "\n  ".join(problems))
    print(f"PASS {label} build worktree matches its patch series")


def check_pinned_submodules(repo: Path) -> None:
    status = run(["git", "-C", str(repo), "submodule", "status", "--recursive"], capture=True)
    if any(line and line[0] != " " for line in status.splitlines()):
        raise SystemExit(f"Uninitialized or mismatched nested dependency in {repo}")
    if run(["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=all"], capture=True).strip():
        raise SystemExit(f"Modified OpenThread source or nested dependency in {repo}")


def tree_files(root: Path) -> dict[str, tuple[bytes, int] | str]:
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
            result[relative.as_posix()] = (path.read_bytes(), path.stat().st_mode & 0o777)
    return result


def main() -> int:
    expected = {
        *ZIGGURAT_MULTI,
        *ZIGGURAT_COMMON,
        "rs-matter-commissioning-probe.patch",
        *RS_MATTER_SERVICE,
        "openthread-hdlc-unix.patch",
        "openthread-hdlc-test.patch",
        "openthread-thread-transport.patch",
        "openthread-fragment-diagnostics.patch",
        "openthread-offmesh-diagnostics.patch",
        *(f"{crate}.patch" for crate in CRATE_PATCHES),
    }
    actual = {path.name for directory in PATCH_DIRS for path in directory.glob("*.patch")}
    if actual != expected:
        missing = sorted(expected - actual)
        unclassified = sorted(actual - expected)
        raise SystemExit(f"Patch inventory mismatch; missing={missing}, unclassified={unclassified}")

    verify_git_series(ROOT / "third-party/acetimec/upstream",
                      [ROOT / "third-party/acetimec/offsets-only.patch"],
                      "AceTime offsets-only engine")

    revision = run(["git", "-C", str(ZIGGURAT), "rev-parse", "HEAD"], capture=True).strip()
    for multipan, names in (("0", ZIGGURAT_COMMON), ("1", ZIGGURAT_MULTI)):
        command = ["python3", str(ROOT / "ziggurat/scripts/check-ziggurat-patches.py"),
                   "--source-revision", revision, "--multipan", multipan]
        build_worktree = ROOT / ("worktrees/ziggurat-multipan" if multipan == "1" else "worktrees/ziggurat-stock")
        if build_worktree.is_dir():
            command.extend(["--build-worktree", str(build_worktree.relative_to(ROOT))])
        subprocess.run(command, cwd=ROOT, check=True)
        print(f"PASS Ziggurat {('multipan' if multipan == '1' else 'stock')} combination ({len(names)} patches)")

    verify_git_series(
        RS_MATTER,
        [patch_path(name) for name in RS_MATTER_SERVICE],
        "rs-matter service series",
    )
    verify_git_series(
        RS_MATTER,
        [patch_path("rs-matter-commissioning-probe.patch"),
         patch_path("rs-matter-commissioning-region.patch")],
        "rs-matter commissioning fixture",
    )
    check_worktree_matches(
        RS_MATTER,
        ROOT / "build/micro-thread-matter-daemon/service-source",
        [patch_path(name) for name in RS_MATTER_SERVICE],
        "rs-matter service series",
    )
    check_worktree_matches(
        RS_MATTER,
        ROOT / "build/micro-thread-matter-daemon/commissioning-source",
        [patch_path("rs-matter-commissioning-probe.patch"),
         patch_path("rs-matter-commissioning-region.patch")],
        "rs-matter commissioning fixture",
    )

    openthread = ROOT / "micro-thread-matter-daemon/dependencies/openthread"
    openthread_patches = [
        patch_path("openthread-hdlc-unix.patch"),
        patch_path("openthread-hdlc-test.patch"),
        patch_path("openthread-thread-transport.patch"),
        patch_path("openthread-fragment-diagnostics.patch"),
        patch_path("openthread-offmesh-diagnostics.patch"),
    ]
    revision = (ROOT / "micro-thread-matter-daemon/dependencies/openthread.revision").read_text().strip()
    if run(["git", "-C", str(openthread), "rev-parse", "HEAD"], capture=True).strip() != revision:
        raise SystemExit("OpenThread checkout differs from its pinned release")
    check_pinned_submodules(openthread)
    verify_git_series(openthread, openthread_patches, "Upstream OpenThread adapter")
    openthread_build = ROOT / "build/openthread-unix/source-mailbox"
    if openthread_build.is_dir():
        check_pinned_submodules(openthread_build)
        check_worktree_matches(openthread, openthread_build, openthread_patches,
                               "OpenThread build copy")

    cargo_roots = sorted((ROOT / "tools/cargo/registry/src").glob("*/"))
    for crate in CRATE_PATCHES:
        patch = patch_path(f"{crate}.patch")
        sources = [path / crate for path in cargo_roots if (path / crate).is_dir()]
        if not sources:
            raise SystemExit(f"Cannot validate {patch.name}: fetch the pinned Cargo crate source first")
        for source in sources:
            verify_snapshot_series(source, [patch], f"Cargo crate {crate}")
        patched = ROOT / "build/patched-crates-mailbox" / crate
        if patched.is_dir():
            check_snapshot_worktree_matches(source, patched, [patch], f"Cargo crate {crate} build copy")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
