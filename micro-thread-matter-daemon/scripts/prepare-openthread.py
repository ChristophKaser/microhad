#!/usr/bin/env python3
"""Prepare a release-pinned OpenThread worktree without touching a radio."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import release_source

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'micro-thread-matter-daemon/dependencies/openthread'
OUTPUT = ROOT / 'build/openthread-unix'
WORKTREE = OUTPUT / 'source-mailbox'
REVISION = (ROOT / 'micro-thread-matter-daemon/dependencies/openthread.revision').read_text().strip()
PATCH_NAMES = ['openthread-hdlc-unix.patch', 'openthread-hdlc-test.patch',
               'openthread-thread-transport.patch', 'openthread-fragment-diagnostics.patch',
               'openthread-offmesh-diagnostics.patch']
PATCHES = [ROOT / 'micro-thread-matter-daemon/patches' / name for name in PATCH_NAMES]


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()


def clean(repo):
    if git(repo, 'status', '--porcelain', '--untracked-files=all'):
        raise SystemExit(f'Preserving modified checkout: {repo}')


def migrate_legacy():
    """Only archive a legacy snapshot whose contents match its original series."""
    backup = OUTPUT / 'source-mailbox-gsdk-backup'
    if backup.exists():
        raise SystemExit(f'Legacy backup already exists; preserving {WORKTREE}')
    clean(WORKTREE)
    # Tree hashes cover every tracked byte and executable bit in the known
    # GSDK snapshot and its original three-patch result, independent of authors.
    base_tree = git(WORKTREE, 'rev-parse', 'refs/tags/mailbox-base^{tree}')
    head_tree = git(WORKTREE, 'rev-parse', 'HEAD^{tree}')
    if (base_tree, head_tree) != (
        '7231b2c98563609dc8a414b08adb443c0814b593',
        '8c6ad25e0cf3946544fca3717b8a6cb073d52740',
    ):
        raise SystemExit(f'Unrecognized legacy source; preserving {WORKTREE}')
    for mode in ('host', 'mips'):
        if (OUTPUT / mode).exists() and (OUTPUT / f'{mode}-gsdk-backup').exists():
            raise SystemExit(f'Cache backup already exists: {mode}-gsdk-backup')
    WORKTREE.rename(backup)
    print(f'Preserved legacy source at {backup}')
    # Compiler/configuration changes must not reuse the GSDK build caches.
    for mode in ('host', 'mips'):
        cache = OUTPUT / mode
        if cache.exists():
            destination = OUTPUT / f'{mode}-gsdk-backup'
            if destination.exists():
                raise SystemExit(f'Cache backup already exists: {destination}')
            cache.rename(destination)


def preserve_previous_release():
    """Keep a clean v2026.10.0 build worktree when moving to the new pin."""
    previous = '53e8322140dde7b26827332100504507cec9cc4e'
    is_current = subprocess.run(
        ['git', '-C', str(WORKTREE), 'merge-base', '--is-ancestor', REVISION, 'HEAD'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    if is_current:
        return
    is_previous = subprocess.run(
        ['git', '-C', str(WORKTREE), 'merge-base', '--is-ancestor', previous, 'HEAD'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    if not is_previous:
        return
    clean(WORKTREE)
    backup = OUTPUT / 'source-mailbox-v2026.10.0-backup'
    if backup.exists():
        raise SystemExit(f'Previous OpenThread backup already exists; preserving {WORKTREE}')
    WORKTREE.rename(backup)
    git(SOURCE, 'worktree', 'repair', str(backup))
    print(f'Preserved previous OpenThread worktree at {backup}')


def main():
    if release_source.enabled():
        release_source.verify('build/openthread-unix/source-mailbox')
        return
    OUTPUT.mkdir(parents=True, exist_ok=True)
    # Initializing a missing dependency is separate from validating its pin.
    if not (SOURCE / '.git').exists():
        git(ROOT, 'submodule', 'update', '--init', 'micro-thread-matter-daemon/dependencies/openthread')
    if git(SOURCE, 'rev-parse', 'HEAD') != REVISION:
        raise SystemExit(f'Expected OpenThread v2026.11.0 revision {REVISION}')
    clean(SOURCE)
    git(SOURCE, 'submodule', 'update', '--init', '--recursive')
    clean(SOURCE)
    legacy = WORKTREE.exists() and subprocess.run(
        ['git', '-C', str(WORKTREE), 'show-ref', '--verify', '--quiet', 'refs/tags/mailbox-base']
    ).returncode == 0
    if legacy:
        migrate_legacy()
    elif WORKTREE.exists():
        preserve_previous_release()
    if not WORKTREE.exists():
        git(SOURCE, 'worktree', 'add', '--detach', str(WORKTREE), REVISION)
    subprocess.run(['python3', str(ROOT / 'scripts/apply-mailbox-series.py'),
                    str(WORKTREE), REVISION, *map(str, PATCHES)], check=True)
    # Initialize the release-pinned Mbed TLS and its framework in the build worktree.
    git(WORKTREE, 'submodule', 'update', '--init', '--recursive')
    clean(WORKTREE)
    print(f'Pinned upstream OpenThread v2026.11.0 ({REVISION})')


if __name__ == '__main__':
    main()
