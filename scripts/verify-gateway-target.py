#!/usr/bin/env python3
"""Inspect gateway ELF files and resolve dynamic dependencies without running them."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from toolchain_config import settings
from stock_runtime import resolve as resolve_runtime


def read(values, binary, *args):
    return subprocess.check_output([values['TARGET_READELF'], *args, str(binary)], text=True)


def needed(values, binary):
    return re.findall(r'\(NEEDED\).*\[([^]]+)\]', read(values, binary, '-d'))


def dynamic_dependencies(values, binary):
    segments = read(values, binary, '-l')
    match = re.search(r'Requesting program interpreter: ([^]]+)', segments)
    if not match or match[1] != '/lib/ld-uClibc.so.0':
        raise ValueError('expected /lib/ld-uClibc.so.0 interpreter: ' + str(binary))
    pending = ['ld-uClibc.so.0', *needed(values, binary)]
    libraries = {}
    while pending:
        name = pending.pop()
        if name in libraries:
            continue
        path = resolve_runtime(values, name)
        header = read(values, path, '-h')
        if not all(item in header for item in ('ELF32', 'big endian', 'MIPS', 'mips1', 'o32')):
            raise ValueError('incompatible runtime ELF: ' + str(path))
        libraries[name] = {'path': str(path), 'bytes': path.stat().st_size}
        pending.extend(needed(values, path))
    # Check strong imports against exports from the entire dependency closure.
    exports, imports = set(), set()
    for path in [binary, *(item['path'] for item in libraries.values())]:
        for line in read(values, path, '--dyn-syms', '--wide').splitlines():
            fields = line.replace('[MIPS PLT]', '').split()
            if len(fields) < 8 or not fields[0].rstrip(':').isdigit():
                continue
            symbol = fields[7].replace('@@', '@')
            if fields[6] == 'UND':
                if fields[4] == 'GLOBAL':
                    imports.add(symbol)
            elif fields[4] in ('GLOBAL', 'WEAK') and fields[5] in ('DEFAULT', 'PROTECTED'):
                exports.update((symbol, symbol.split('@')[0]))
    missing = sorted(imports - exports)
    if missing:
        raise ValueError('unresolved dynamic symbols: ' + ', '.join(missing))
    return {'interpreter': match[1], 'needed': needed(values, binary), 'libraries': libraries,
            'runtime_source': 'stock image', 'additional_runtime_bytes': 0}


def verify(binary, link_mode='static'):
    values = settings()
    binary = Path(binary).resolve()
    header, attributes = read(values, binary, '-h'), read(values, binary, '-A')
    if not all(item in header for item in ('ELF32', 'big endian', 'MIPS', 'mips1', 'o32')):
        raise ValueError('expected ELF32 big-endian MIPS-I/O32: ' + str(binary))
    if 'Soft float' not in attributes:
        raise ValueError('expected soft-float executable: ' + str(binary))
    report = {'binary': str(binary.resolve()), 'link_mode': link_mode, 'bytes': binary.stat().st_size}
    if link_mode == 'static':
        if 'INTERP' in read(values, binary, '-l') or 'There is no dynamic section' not in read(values, binary, '-d'):
            raise ValueError('expected static soft-float executable: ' + str(binary))
    else:
        if values['TOOLCHAIN'] != 'rsdk':
            raise ValueError('dynamic verification requires TOOLCHAIN=rsdk')
        if not re.search(r'Type:\s+EXEC\b', header):
            raise ValueError('expected non-PIE ET_EXEC dynamic executable: ' + str(binary))
        report.update(dynamic_dependencies(values, binary))
    decoded = subprocess.check_output(['llvm-objdump-18', '--disassemble', '--mcpu=mips32r2', str(binary)], text=True)
    counts = {op: len(re.findall(r'\t' + op + r'\s', decoded)) for op in ('rdhwr', 'teq', 'sync', 'll', 'sc', 'lwl', 'lwr', 'swl', 'swr')}
    if counts['rdhwr'] or counts['teq']:
        raise ValueError('unsupported stock-kernel instructions: ' + str(counts))
    report['instructions'] = counts
    if link_mode == 'dynamic':
        parts = list(binary.parts)
        if 'dynamic' in parts:
            parts.remove('dynamic')
            parts = [part.replace('lexra-stock-uclibc-dynamic', 'lexra-stock-uclibc') for part in parts]
            static = Path(*parts)
            report['static_binary'] = str(static)
            report['static_bytes'] = static.stat().st_size if static.is_file() else None
        report['runtime_bytes'] = sum(item['bytes'] for item in report['libraries'].values())
        report['bytes_with_runtime'] = report['bytes'] + report['runtime_bytes']
        Path(str(binary) + '.linkage.json').write_text(json.dumps(report, indent=2) + '\n')
    print(str(binary) + ': verified ' + link_mode + ' BE MIPS-I/O32 soft-float; instructions=' + str(counts))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--link-mode', choices=('static', 'dynamic'), default='static')
    parser.add_argument('executables', nargs='+')
    args = parser.parse_args()
    try:
        for binary in args.executables:
            verify(binary, args.link_mode)
    except ValueError as error:
        raise SystemExit(str(error))
