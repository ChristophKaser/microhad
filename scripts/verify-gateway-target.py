#!/usr/bin/env python3
"""Inspect a stock-kernel executable without running it or accessing a device."""
import re
import subprocess
import sys
from toolchain_config import settings


def verify(binary):
    values = settings()
    def read(*args):
        return subprocess.check_output([values['TARGET_READELF'], *args, binary], text=True)
    header, attributes = read('-h'), read('-A')
    if not all(item in header for item in ('ELF32', 'big endian', 'MIPS', 'mips1', 'o32')):
        raise ValueError('expected ELF32 big-endian MIPS-I/O32: ' + binary)
    if 'Soft float' not in attributes or 'INTERP' in read('-l') or 'There is no dynamic section' not in read('-d'):
        raise ValueError('expected static soft-float executable: ' + binary)
    decoded = subprocess.check_output(['llvm-objdump-18', '--disassemble', '--mcpu=mips32r2', binary], text=True)
    counts = {op: len(re.findall(r'\t' + op + r'\s', decoded)) for op in ('rdhwr', 'teq', 'sync', 'll', 'sc', 'lwl', 'lwr', 'swl', 'swr')}
    if counts['rdhwr'] or counts['teq']:
        raise ValueError('unsupported stock-kernel instructions: ' + str(counts))
    print(binary + ': verified static BE MIPS-I/O32 soft-float; instructions=' + str(counts))


if __name__ == '__main__':
    try:
        if len(sys.argv) < 2:
            raise ValueError('usage: verify-gateway-target.py EXECUTABLE ...')
        for binary in sys.argv[1:]:
            verify(binary)
    except ValueError as error:
        raise SystemExit(str(error))
