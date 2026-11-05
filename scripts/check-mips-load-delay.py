#!/usr/bin/env python3
"""Reject MIPS delay-slot loads across call/indirect-jump boundaries."""
import argparse
from pathlib import Path
import re
import subprocess
from toolchain_config import settings

LOADS = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwl', 'lwr', 'll', 'ld', 'lwu', 'lwc1', 'ldc1'}
TRANSFER = {'j', 'b', 'bal', 'jal', 'jalr', 'jr', 'jalx'}
INSTRUCTION = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)', re.I)


def hazards(text):
    previous = None
    current_symbol = None
    result = []
    for line in text.splitlines():
        instruction = INSTRUCTION.match(line)
        if instruction:
            address, mnemonic = instruction.groups()
            address = int(address, 16)
            if previous and address == previous[0] + 4 and previous[1] in TRANSFER and mnemonic in LOADS:
                # A direct intra-function jump has a known successor; LLVM
                # schedules it locally. Calls/indirect jumps cross that boundary.
                target = re.search(r'<(.+)>', previous[2])
                local_jump = (previous[1] in ('j', 'b') and current_symbol and target
                              and target[1].startswith(current_symbol + '+0x'))
                if not local_jump:
                    result.append(line.strip())
            previous = address, mnemonic, line
        elif line.endswith('>:'):
            previous = None
            symbol = re.search(r'<(.+)>:', line)
            current_symbol = symbol[1] if symbol else None
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
        assert not hazards(' 100: 10400008 beqz v0,124\n 104: 8fb60020 lw s6,32(sp)\n 108: 12c00008 beqz s6,12c')
        assert hazards(' 100: 08000080 j 200 <other>\n 104: 8fa50018 lw a1,24(sp)')
        assert not hazards('00000100 <local>:\n 100: 08000080 j 200 <local+0x100>\n 104: 8fa50018 lw a1,24(sp)')
        print('PASS detects the captured call-slot hazard and accepts separated loads')
    values = settings()
    objdump = values['TARGET_TOOLCHAIN_ROOT'] / 'bin' / (values['TARGET_PREFIX'] + 'objdump')
    artifacts = []
    for path in args.artifacts:
        if path.is_dir():
            archives = sorted(path.rglob('*.a'))
            if not archives:
                raise SystemExit('No archives found: ' + str(path))
            artifacts.extend(archives)
        else:
            artifacts.append(path)
    for artifact in artifacts:
        text = subprocess.check_output([objdump, '-d', artifact], text=True)
        found = hazards(text)
        if found:
            raise SystemExit(str(artifact) + ': load in delay slot:\n' + '\n'.join(found[:20]))
        if not any(INSTRUCTION.match(line) for line in text.splitlines()):
            headers = subprocess.check_output([objdump, '-h', artifact], text=True).splitlines()
            for index, line in enumerate(headers[:-1]):
                section = re.match(r'^\s*\d+\s+\S+\s+([0-9a-f]+)\s', line, re.I)
                if section and int(section[1], 16) and 'CODE' in headers[index + 1]:
                    raise SystemExit('Executable section not decoded: ' + str(artifact))
            print('PASS no executable code:', artifact)
            continue
        print('PASS no loads across call/indirect-jump delay slots:', artifact)


if __name__ == '__main__':
    main()
