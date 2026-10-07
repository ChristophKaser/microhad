#!/usr/bin/env python3
"""Validate every tracked patch and active source/patch combination offline."""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PATCH_DIR = ROOT / "patches"
ZIGGURAT = ROOT / "ziggurat"
RS_MATTER = ROOT / "dependencies/rs-matter"
CPC = ROOT / "cpc-daemon"

ZIGGURAT_COMMON = ["ziggurat-stock-compat.patch", "ziggurat-syslog.patch", "ziggurat-syslog-fragments.patch"]
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
]
RS_MATTER_PROBE = [
    "rs-matter-probe-spake2p.patch",
    "rs-matter-probe-peer-identity.patch",
    "rs-matter-probe-attribute-notify.patch",
    "rs-matter-probe-icd-capture.patch",
    "rs-matter-probe-sessionless-icd.patch",
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


def tree_files(root: Path) -> dict[str, tuple[bytes, int] | str]:
    result: dict[str, tuple[bytes, int] | str] = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if relative.parts[0] == ".git" or relative == Path("Cargo.lock"):
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
        *RS_MATTER_PROBE,
        "cpc-stock.patch",
        "cpc-tcp-bus.patch",
        "openthread-hdlc-unix.patch",
        "openthread-hdlc-test.patch",
        "openthread-thread-transport.patch",
        *(f"{crate}.patch" for crate in CRATE_PATCHES),
    }
    actual = {path.name for path in PATCH_DIR.glob("*.patch")}
    if actual != expected:
        missing = sorted(expected - actual)
        unclassified = sorted(actual - expected)
        raise SystemExit(f"Patch inventory mismatch; missing={missing}, unclassified={unclassified}")

    revision = run(["git", "-C", str(ZIGGURAT), "rev-parse", "HEAD"], capture=True).strip()
    for multipan, names in (("0", ZIGGURAT_COMMON), ("1", ZIGGURAT_MULTI)):
        command = ["python3", str(ROOT / "scripts/check-ziggurat-patches.py"),
                   "--source-revision", revision, "--multipan", multipan]
        build_worktree = ROOT / ("worktrees/ziggurat-hdlc" if multipan == "1" else "worktrees/ziggurat")
        if build_worktree.is_dir():
            command.extend(["--build-worktree", str(build_worktree.relative_to(ROOT))])
        subprocess.run(command, cwd=ROOT, check=True)
        print(f"PASS Ziggurat {('multipan' if multipan == '1' else 'stock')} combination ({len(names)} patches)")

    verify_git_series(
        RS_MATTER,
        [PATCH_DIR / name for name in RS_MATTER_PROBE],
        "rs-matter probe series",
    )
    verify_git_series(
        RS_MATTER,
        [PATCH_DIR / "rs-matter-commissioning-probe.patch"],
        "rs-matter commissioning fixture",
    )
    check_worktree_matches(
        RS_MATTER,
        ROOT / "build/rs-matter/probe-source",
        [PATCH_DIR / name for name in RS_MATTER_PROBE],
        "rs-matter probe series",
    )
    check_worktree_matches(
        RS_MATTER,
        ROOT / "build/rs-matter/controller-source",
        [PATCH_DIR / "rs-matter-commissioning-probe.patch"],
        "rs-matter commissioning fixture",
    )

    cpc_series = [PATCH_DIR / "cpc-tcp-bus.patch", PATCH_DIR / "cpc-stock.patch"]
    verify_git_series(CPC, cpc_series, "CPC daemon")
    check_worktree_matches(CPC, ROOT / "worktrees/cpc-daemon", cpc_series, "CPC daemon")

    openthread = ROOT / "dependencies/gecko-sdk/util/third_party/openthread"
    openthread_patches = [
        PATCH_DIR / "openthread-hdlc-unix.patch",
        PATCH_DIR / "openthread-hdlc-test.patch",
        PATCH_DIR / "openthread-thread-transport.patch",
    ]
    verify_snapshot_series(openthread, openthread_patches, "Gecko SDK OpenThread adapter")
    openthread_build = ROOT / "build/openthread-unix/source-mailbox"
    if openthread_build.is_dir():
        check_snapshot_worktree_matches(openthread, openthread_build, openthread_patches,
                                        "OpenThread build copy")

    cargo_roots = sorted((ROOT / "tools/cargo/registry/src").glob("*/"))
    for crate in CRATE_PATCHES:
        patch = PATCH_DIR / f"{crate}.patch"
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
