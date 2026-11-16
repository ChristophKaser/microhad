#!/usr/bin/env python3
"""Archive matching MIPS deployment symbols under the deployed file's SHA-256."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile


def sections(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x01\x02':
        raise ValueError(f'{path}: expected big-endian ELF32')
    shoff = struct.unpack_from('>I', data, 32)[0]
    entsize, count, names_index = struct.unpack_from('>HHH', data, 46)
    if entsize < 40 or not count or names_index >= count or shoff + entsize * count > len(data):
        raise ValueError(f'{path}: invalid section table')
    headers = [struct.unpack_from('>10I', data, shoff + i * entsize) for i in range(count)]
    names_header = headers[names_index]
    names = data[names_header[4]:names_header[4] + names_header[5]]
    allocated = {}
    symtab = False
    for name_offset, kind, flags, addr, offset, size, _, _, _, _ in headers:
        if name_offset >= len(names):
            raise ValueError(f'{path}: invalid section name')
        end = names.find(b'\0', name_offset)
        if end < 0:
            raise ValueError(f'{path}: invalid section name')
        name = names[name_offset:end]
        if kind == 2 and size:
            symtab = True
        if flags & 2 and size:
            if kind != 8 and offset + size > len(data):
                raise ValueError(f'{path}: truncated section')
            allocated[name] = (kind, addr, size, b'' if kind == 8 else data[offset:offset + size])
    return allocated, symtab


def main():
    if len(sys.argv) != 4:
        raise ValueError('usage: archive-deploy-symbols.py NAME BINARY ARCHIVE_DIR')
    name, binary, archive = sys.argv[1:]
    if not name or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in name):
        raise ValueError('invalid service name')
    binary = Path(binary)
    symbols = Path(str(binary) + '.elf')
    deployed, _ = sections(binary)
    unstripped, has_symbols = sections(symbols)
    if not has_symbols or not deployed or deployed != unstripped:
        raise ValueError(f'{symbols}: missing symbols or does not match {binary}')
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    folder = Path(archive) / name / digest
    folder.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(folder, 0o700)
    target = folder / 'unstripped.elf'
    with tempfile.NamedTemporaryFile(dir=folder, delete=False) as f:
        temp = Path(f.name)
        try:
            with symbols.open('rb') as src:
                shutil.copyfileobj(src, f)
        except BaseException:
            temp.unlink(missing_ok=True)
            raise
    os.replace(temp, target)
    print(f'Preserved symbols: {target} (binary sha256 {digest})')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        print(f'Symbol archive failed: {error}', file=sys.stderr)
        sys.exit(1)
