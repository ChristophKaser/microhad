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

ZIGGURAT_COMMON = ["ziggurat-stock-compat.patch", "ziggurat-syslog.patch"]
ZIGGURAT_MULTI = [
    "ziggurat-hdlc-multipan.patch",
    "ziggurat-network-migration.patch",
    *ZIGGURAT_COMMON,
    "ziggurat-cobs-unix.patch",
    "ziggurat-poll-diagnostics.patch",
    "ziggurat-parent-announcement.patch",
    "ziggurat-fresh-join-replay.patch",
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
        run(["git", "-C", str(repo), "apply", "--check", str(patch)])
        run(["git", "-C", str(repo), "apply", str(patch)])


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


def check_worktree_matches(source: Path, worktree: Path, patches: list[Path], label: str) -> None:
    if not worktree.is_dir():
        return
    revision = run(["git", "-C", str(source), "rev-parse", "HEAD"], capture=True).strip()
    actual_revision = run(["git", "-C", str(worktree), "rev-parse", "HEAD"], capture=True).strip()
    if actual_revision != revision:
        print(f"SKIP {label} build worktree is based on {actual_revision}, current pin is {revision}")
        return
    with tempfile.TemporaryDirectory(prefix="patch-worktree-coverage-") as temporary:
        expected = Path(temporary) / "source"
        temporary_worktree(source, revision, expected)
        try:
            apply_series(expected, patches)
            expected_files = tree_files(expected)
            actual_files = tree_files(worktree)
        finally:
            remove_worktree(source, expected)
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
        "rs-matter-commissioning-probe.patch",
        *RS_MATTER_PROBE,
        "cpc-stock.patch",
        "openthread-hdlc-unix.patch",
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

    gateway_patch = ROOT / (
        "rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/cpcd/tcp-bus.patch"
    )
    cpc_series = [gateway_patch, PATCH_DIR / "cpc-stock.patch"]
    verify_git_series(CPC, cpc_series, "CPC daemon")
    check_worktree_matches(CPC, ROOT / "worktrees/cpc-daemon", cpc_series, "CPC daemon")

    openthread = ROOT / "dependencies/gecko-sdk/util/third_party/openthread"
    openthread_patch = PATCH_DIR / "openthread-hdlc-unix.patch"
    run(["git", "apply", "--check", str(openthread_patch)], cwd=openthread)
    print("PASS Gecko SDK OpenThread adapter")
    openthread_build = ROOT / "build/openthread-unix/source"
    if openthread_build.is_dir():
        changed_paths = [
            line.removeprefix("+++ b/")
            for line in openthread_patch.read_text().splitlines()
            if line.startswith("+++ b/")
        ]
        with tempfile.TemporaryDirectory(prefix="openthread-patch-coverage-") as temporary:
            expected = Path(temporary)
            for relative in changed_paths:
                (expected / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(openthread / relative, expected / relative)
            run(["git", "apply", str(openthread_patch)], cwd=expected)
            mismatches = [
                relative
                for relative in changed_paths
                if (expected / relative).read_bytes() != (openthread_build / relative).read_bytes()
            ]
        if mismatches:
            raise SystemExit("OpenThread patch coverage failed for: " + ", ".join(mismatches))
        print("PASS OpenThread build copy matches patched source files")

    cargo_roots = sorted((ROOT / "tools/cargo/registry/src").glob("*/"))
    for crate in CRATE_PATCHES:
        patch = PATCH_DIR / f"{crate}.patch"
        sources = [path / crate for path in cargo_roots if (path / crate).is_dir()]
        if sources:
            for source in sources:
                run(["git", "apply", "--check", str(patch)], cwd=source)
        else:
            patched = ROOT / "build/patched-crates" / crate
            if not patched.is_dir():
                raise SystemExit(f"Cannot validate {patch.name}: fetch the pinned Cargo crate source first")
            run(["git", "apply", "--reverse", "--check", str(patch)], cwd=patched)
        print(f"PASS Cargo crate {crate}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
