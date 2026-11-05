#!/usr/bin/env python3
"""Explicitly install pinned host LLVM tools; application builds never install tools."""
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '23.1.5'
ARCHIVE = f'LLVM-{VERSION}-Linux-X64.tar.xz'
SHA256 = 'f72edb0b36580ece7f52e23858f00c18fad4e87395ea7ad05d7e55c39f8fc78a'
URL = f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{ARCHIVE}'


def main():
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        raise SystemExit('Pinned LLVM installation requires Linux x86_64')
    destination = ROOT / 'tools' / f'llvm-{VERSION}'
    if destination.exists():
        for tool in ('clang', 'clang++', 'llvm-ar', 'llvm-strip'):
            text = subprocess.check_output([destination / 'bin' / tool, '--version'], text=True)
            if VERSION not in text:
                raise SystemExit('Existing LLVM installation has an unexpected version')
        print('Verified existing LLVM', VERSION)
        return
    destination.parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.llvm-install-', dir=destination.parent) as temporary:
        staging = Path(temporary)
        archive = staging / ARCHIVE
        print('Downloading', URL, flush=True)
        with urllib.request.urlopen(URL, timeout=60) as source, archive.open('wb') as output:
            shutil.copyfileobj(source, output)
        with archive.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        if digest != SHA256:
            raise SystemExit('LLVM archive SHA-256 mismatch')
        extracted = staging / 'extracted'
        extracted.mkdir()
        with tarfile.open(archive) as source:
            source.extractall(extracted, filter='data')
        roots = list(extracted.iterdir())
        installation = roots[0] if len(roots) == 1 and (roots[0] / 'bin/clang').exists() else extracted
        for tool in ('clang', 'clang++', 'llvm-ar', 'llvm-strip'):
            text = subprocess.check_output([installation / 'bin' / tool, '--version'], text=True)
            if VERSION not in text:
                raise SystemExit('Extracted LLVM has an unexpected version')
        os.rename(installation, destination)
    print('Installed', destination)


if __name__ == '__main__':
    main()
