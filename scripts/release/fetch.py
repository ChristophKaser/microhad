#!/usr/bin/env python3
"""Fetch the pinned build inputs omitted from a release source archive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

from common import inventory, sha

ROOT = Path(__file__).resolve().parents[2]


def tree_digest(root, exclude=()):
    paths = (p for p in root.rglob('*') if '.git' not in p.relative_to(root).parts
             and p.name not in ('.cargo-ok', '.cargo-checksum.json')
             and not any(p == root / n or p.is_relative_to(root / n) for n in exclude))
    return hashlib.sha256(json.dumps(inventory(root, paths), sort_keys=True).encode()).hexdigest()


def download(url, destination, checksum):
    if destination.exists():
        if sha(destination) != checksum:
            raise ValueError('Download checksum mismatch: ' + str(destination))
        return
    if not url.startswith('https://'):
        raise ValueError('Expected HTTPS download: ' + url)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.part')
    print('Fetching ' + url, flush=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as out:
            shutil.copyfileobj(response, out)
        if sha(temporary) != checksum:
            raise ValueError('Download checksum mismatch: ' + url)
        temporary.rename(destination)
    finally:
        temporary.unlink(missing_ok=True)


def extract(archive, destination, strip_root=False):
    # Preserve the original text of safe relative symlinks (data_filter can
    # normalize a trailing slash, which would change our source inventory).
    def safe(member, path):
        checked = tarfile.data_filter(member, path)
        return checked.replace(linkname=member.linkname) if checked.issym() else checked
    with tarfile.open(archive) as source:
        source.extractall(destination, filter=safe)
    if strip_root:
        children = list(destination.iterdir())
        if len(children) != 1 or not children[0].is_dir():
            raise ValueError('Expected one archive root: ' + str(archive))
        child = children[0]
        for path in child.iterdir():
            path.rename(destination / path.name)
        child.rmdir()


def verify_tree(root, record):
    if tree_digest(root, record.get('exclude', ())) != record['sha256']:
        raise ValueError('Fetched source mismatch: ' + record['destination'])


def apply_patch(root, patch):
    # Keep Git from discovering a surrounding developer checkout.
    subprocess.run(['git', 'init', '--quiet', str(root)], check=True)
    try:
        subprocess.run(['git', '-C', str(root), 'apply', str(patch)], check=True)
    finally:
        shutil.rmtree(root / '.git')


def fetch_tree(root, record):
    destination = root / record['destination']
    # git archive emits empty directories for gitlinks. They are placeholders,
    # not existing source checkouts; never replace a populated directory.
    if destination.is_dir() and not destination.is_symlink() and not any(destination.iterdir()):
        destination.rmdir()
    if destination.exists():
        verify_tree(destination, record)
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.fetch-', dir=destination.parent) as directory:
        temporary = Path(directory)
        result = temporary / 'source'
        result.mkdir()
        if record['kind'] == 'git':
            if not record['url'].startswith('https://'):
                raise ValueError('Expected HTTPS Git source')
            repo = temporary / 'repo.git'
            subprocess.run(['git', 'init', '--bare', '--quiet', str(repo)], check=True)
            subprocess.run(['git', '-C', str(repo), 'fetch', '--quiet', '--depth=1',
                            record['url'], record['revision']], check=True)
            # Export the committed bytes, including files upstream marks as
            # export-ignore, without substituting commit data into export-subst.
            (repo / 'info/attributes').write_text('* -export-ignore -export-subst\n')
            archive = temporary / 'source.tar'
            subprocess.run(['git', '-C', str(repo), 'archive', '--format=tar',
                            '--output=' + str(archive), record['revision']], check=True)
            extract(archive, result)
        elif record['kind'] == 'patched':
            shutil.copytree(root / record['base'], result, dirs_exist_ok=True, symlinks=True)
            apply_patch(result, root / record['patch'])
        elif record['kind'] == 'crate':
            archive = root / '.release/downloads' / (record['name'] + '-' + record['version'] + '.crate')
            download(record['url'], archive, record['archive_sha256'])
            extract(archive, result, strip_root=True)
            apply_patch(result, root / record['patch'])
        else:
            raise ValueError('Unknown fetch recipe: ' + record['kind'])
        for name, source in record.get('overlays', {}).items():
            shutil.copy2(root / source, result / name)
        verify_tree(result, record)
        result.rename(destination)
    print('Verified ' + record['destination'], flush=True)


def fetch_toolchain(root):
    records = json.loads((root / 'config/crosstool-ng/sources.json').read_text())
    for name, record in records.items():
        download(record['url'], root / 'build/toolchain-proof/downloads' / name, record['sha256'])


def fetch(root, env, toolchain, toolchain_archives=True):
    records = json.loads((root / '.release/fetch-manifest.json').read_text())
    for record in records['sources']:
        fetch_tree(root, record)
    if toolchain_archives:
        fetch_toolchain(root)
    probe = subprocess.run(['rustc', '+' + toolchain, '--print', 'sysroot'], env=env,
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if probe.returncode or not (Path(probe.stdout.strip()) / 'lib/rustlib/src/rust/library/Cargo.toml').is_file():
        subprocess.run(['rustup', 'toolchain', 'install', toolchain, '--profile', 'minimal',
                        '--component', 'rust-src'], env=env, check=True)
    # Lockfiles and Cargo's registry checksums pin transitive dependencies.
    for manifest in ('micro-thread-matter-daemon/Cargo.toml', 'worktrees/ziggurat-multipan/Cargo.toml'):
        subprocess.run(['cargo', '+' + toolchain, 'fetch', '--locked', '--manifest-path', manifest],
                       cwd=root, env=env, check=True)
    subprocess.run(['npm', 'ci', '--ignore-scripts', '--no-audit'], cwd=root / 'microhad/webui', env=env, check=True)


def main():
    sys.path.insert(0, str(ROOT / 'scripts'))
    import release_source
    release_source.verify()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cargo-home', type=Path, default=Path(os.environ.get('CARGO_HOME', ROOT / 'tools/cargo')))
    parser.add_argument('--rustup-home', type=Path, default=Path(os.environ.get('RUSTUP_HOME', ROOT / 'tools/rustup')))
    parser.add_argument('--without-toolchain', action='store_true', help='use a separately installed Lexra compiler')
    args = parser.parse_args()
    import importlib.util
    spec = importlib.util.spec_from_file_location('builder', ROOT / 'scripts/build-release.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    env = builder.environment(ROOT, 1, args.cargo_home.resolve(), args.rustup_home.resolve(),
                              release_source.metadata()['source_date_epoch'])
    config = dict(line.split('=', 1) for line in (ROOT / 'config/toolchains.mk').read_text().splitlines()
                  if line and not line.startswith('#'))
    fetch(ROOT, env, config['RUST_TOOLCHAIN'], not args.without_toolchain)
    print('Pinned source downloads complete')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit('Source fetch failed: ' + str(error))
