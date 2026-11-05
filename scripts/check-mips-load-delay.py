#!/usr/bin/env python3
"""Reject loads in MIPS control-transfer delay slots (including call boundaries)."""
import argparse
from pathlib import Path
import re
import subprocess
from toolchain_config import settings

LOADS = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwl', 'lwr', 'll', 'ld', 'lwu', 'lwc1', 'ldc1'}
TRANSFER = re.compile(r'^(?:j|jal|jr|jalr|b|bal|beq.*|bne.*|bgez.*|bgtz.*|blez.*|bltz.*|bc[012].*)$')
INSTRUCTION = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)', re.I)


def hazards(text):
    previous = None
    result = []
    for line in text.splitlines():
        instruction = INSTRUCTION.match(line)
        if instruction:
            address, mnemonic = instruction.groups()
            address = int(address, 16)
            if previous and address == previous[0] + 4 and TRANSFER.match(previous[1]) and mnemonic in LOADS:
                result.append(line.strip())
            previous = address, mnemonic
        elif line.endswith('>:'):
            previous = None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifacts', nargs='*', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        assert hazards(' 100: 0c000080 jal 200\n 104: 8fa50018 lw a1,24(sp)')
        assert not hazards(' 100: 8fa50018 lw a1,24(sp)\n 104: 00000000 nop\n 108: 0c000080 jal 200\n 10c: 00000000 nop')
        assert hazards(' 100: 03e00008 jr ra\n 104: 8fa50018 lw a1,24(sp)')
        print('PASS detects the captured call-slot hazard and accepts separated loads')
    values = settings()
    objdump = values['TARGET_TOOLCHAIN_ROOT'] / 'bin' / (values['TARGET_PREFIX'] + 'objdump')
    for artifact in args.artifacts:
        text = subprocess.check_output([objdump, '-d', artifact], text=True)
        found = hazards(text)
        if found:
            raise SystemExit(str(artifact) + ': load in delay slot:\n' + '\n'.join(found[:20]))
        if not any(INSTRUCTION.match(line) for line in text.splitlines()):
            raise SystemExit('No instructions decoded: ' + str(artifact))
        print('PASS no loads in control-transfer delay slots:', artifact)


if __name__ == '__main__':
    main()
