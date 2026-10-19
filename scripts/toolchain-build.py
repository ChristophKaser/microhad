#!/usr/bin/env python3
"""Invalidate generated target caches when their compiler selection changes."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from toolchain_config import ROOT, settings


def ensure(directory):
    directory = Path(directory).absolute()
    if directory.resolve() != directory:
        raise ValueError("target cache path must not contain symlinks or parent traversal")
    allowed = (ROOT / 'build', ROOT / 'microhad/build')
    if directory.is_symlink() or not any(directory.is_relative_to(base) and directory != base for base in allowed):
        raise ValueError('expected a component target directory under build/: ' + str(directory))
    values = settings()
    evidence = {key: str(values[key]) for key in ('TOOLCHAIN', 'LINK_MODE', 'TARGET_TOOLCHAIN_ROOT', 'TARGET_PREFIX',
                'TARGET_CC', 'TARGET_CXX', 'TARGET_AR', 'TARGET_NM', 'TARGET_STRIP', 'TARGET_READELF')}
    for key in ('TARGET_CC', 'TARGET_CXX', 'TARGET_AR', 'TARGET_NM', 'TARGET_STRIP', 'TARGET_READELF'):
        tool = Path(values[key]).resolve()
        evidence[key + '_identity'] = [str(tool), tool.stat().st_mtime_ns, tool.stat().st_size]
    evidence['compiler_version'] = subprocess.check_output([values['TARGET_CC'], '--version'], text=True)
    evidence['sysroot'] = subprocess.check_output([values['TARGET_CC'], '-print-sysroot'], text=True).strip()
    libraries = ('libc.a', 'libpthread.a', 'libstdc++.a')
    if values['LINK_MODE'] == 'dynamic':
        libraries += ('libc.so.0', 'libpthread.so.0', 'librt.so.0', 'libstdc++.so.6', 'libgcc_s.so.1', 'ld-uClibc.so.0')
    for name in libraries:
        archive = Path(subprocess.check_output([values['TARGET_CC'], '-print-file-name=' + name], text=True).strip()).resolve()
        evidence[name] = [str(archive), archive.stat().st_mtime_ns, archive.stat().st_size]
        if values['TOOLCHAIN'] == 'crosstool-ng' and not archive.is_relative_to(values['TARGET_TOOLCHAIN_ROOT']):
            raise ValueError('candidate runtime archive outside selected toolchain: ' + str(archive))
    fingerprint = hashlib.sha256(json.dumps(evidence, sort_keys=True).encode()).hexdigest()
    marker = directory / '.toolchain'
    if marker.exists() and marker.read_text().strip() == fingerprint:
        return
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    marker.write_text(fingerprint + '\n')
    (directory / 'toolchain-provenance.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    try:
        ensure(sys.argv[1])
    except (ValueError, FileNotFoundError) as error:
        raise SystemExit(str(error))
