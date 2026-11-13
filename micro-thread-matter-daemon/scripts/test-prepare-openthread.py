#!/usr/bin/env python3
"""Exercise the previous-release worktree migration without touching build output."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'micro-thread-matter-daemon/dependencies/openthread'
PREVIOUS = '53e8322140dde7b26827332100504507cec9cc4e'


def main():
    spec = importlib.util.spec_from_file_location('prepare_openthread', Path(__file__).with_name('prepare-openthread.py'))
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    with tempfile.TemporaryDirectory(prefix='ot-upgrade-test-') as directory:
        base = Path(directory)
        repo, output = base / 'repo', base / 'output'
        output.mkdir()
        subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout', str(SOURCE), str(repo)], check=True)
        worktree = output / 'source-mailbox'
        subprocess.run(['git', '-C', str(repo), 'worktree', 'add', '--quiet', '--detach', str(worktree), PREVIOUS],
                       check=True)
        prepare.SOURCE, prepare.OUTPUT, prepare.WORKTREE = repo, output, worktree

        readme = worktree / 'README.md'
        readme.write_text('uncommitted content\n')
        try:
            prepare.preserve_previous_release()
        except SystemExit:
            assert readme.read_text() == 'uncommitted content\n'
        else:
            raise AssertionError('Dirty previous worktree was not preserved')
        subprocess.run(['git', '-C', str(worktree), 'restore', 'README.md'], check=True)

        backup = output / 'source-mailbox-v2026.10.0-backup'
        backup.mkdir()
        try:
            prepare.preserve_previous_release()
        except SystemExit:
            assert worktree.is_dir() and backup.is_dir()
        else:
            raise AssertionError('Existing backup was not preserved')
        backup.rmdir()
        prepare.preserve_previous_release()
        assert backup.is_dir() and not worktree.exists()
        assert subprocess.check_output(['git', '-C', str(backup), 'rev-parse', 'HEAD'], text=True).strip() == PREVIOUS
        subprocess.run(['git', '-C', str(repo), 'worktree', 'add', '--quiet', '--detach', str(worktree),
                        prepare.REVISION], check=True)
        prepare.preserve_previous_release()
        assert worktree.is_dir() and backup.is_dir()
    print('PASS dirty worktree preserved, previous release backed up, current worktree retained')


if __name__ == '__main__':
    main()
