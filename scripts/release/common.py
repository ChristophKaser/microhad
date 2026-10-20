"""Release archive/installer primitives; no compiler or device access."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(root, paths=None):
    root = Path(root)
    result = {}
    for path in sorted(paths if paths is not None else root.rglob('*')):
        if path.is_dir() and not path.is_symlink():
            continue
        name = path.relative_to(root).as_posix()
        if path.is_symlink():
            if not path.resolve().is_relative_to(root.resolve()):
                raise ValueError('External archive symlink: ' + name)
            result[name] = {'symlink': os.readlink(path)}
        else:
            result[name] = {'sha256': sha(path), 'executable': bool(path.stat().st_mode & 0o111)}
    return result


def verify_inventory(root, records):
    root = Path(root).resolve()
    for name, record in records.items():
        if Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('Unsafe package path: ' + name)
        path = root / name
        if not path.resolve().is_relative_to(root):
            raise ValueError('External package path: ' + name)
        if 'symlink' in record:
            if not path.is_symlink() or os.readlink(path) != record['symlink']:
                raise ValueError('Package symlink mismatch: ' + name)
        elif path.is_symlink() or not path.is_file() or sha(path) != record['sha256']:
            raise ValueError('Package checksum mismatch: ' + name)
        elif bool(path.stat().st_mode & 0o111) != record['executable']:
            raise ValueError('Package mode mismatch: ' + name)


def verify_elf(path, readelf='readelf'):
    def read(flag):
        return subprocess.check_output([str(readelf), flag, str(path)], text=True)
    header, attributes = read('-h'), read('-A')
    if not all(item in header for item in ('ELF32', 'big endian', 'MIPS', 'mips1', 'o32')):
        raise ValueError('Expected ELF32 big-endian MIPS-I/O32: ' + str(path))
    if 'Soft float' not in attributes or 'INTERP' in read('-l') or '(NEEDED)' in read('-d'):
        raise ValueError('Expected static soft-float executable: ' + str(path))


def verify_binary(root, readelf='readelf'):
    root = Path(root)
    metadata = json.loads((root / '.release/binary-manifest.json').read_text())
    verify_inventory(root, metadata['files'])
    for name in ('microhad', 'microhad-ezsp', 'mthread-matter', 'ziggurat', 'gateway-maintenance'):
        verify_elf(root / 'bin' / name, readelf)
    manifest = (root / 'bin/microhad.matter-manifest').read_text().splitlines()
    expected = ['format=microhad-build-v1', 'ziggurat=1', 'matter=1', 'binary_sha256=' + sha(root / 'bin/microhad')]
    if sorted(manifest) != sorted(expected):
        raise ValueError('MicroHAD Matter manifest mismatch')
    return metadata
