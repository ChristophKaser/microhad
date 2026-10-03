#!/usr/bin/env python3
"""Check Make feature selection without compiling, fetching, or touching a device."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def selected(arguments):
    # A private additional makefile target prints the selected public build contract.
    rule = '''.PHONY: print-matter-selection
print-matter-selection:
\t@echo $(MATTER) $(HOST_BUILD) $(MIPS_BUILD)
\t@echo $(HOST_CXXFLAGS)
'''
    return subprocess.run(['make', '--no-print-directory', '-f', 'Makefile', '-f', '-',
                           'print-matter-selection', *arguments], cwd=ROOT, input=rule,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    for args, host, mips in (
        ([], 'build/host', 'build/mips'),
        (['ZIGGURAT=0', 'MATTER=0'], 'build/host', 'build/mips'),
        (['ZIGGURAT=1', 'MATTER=0'], 'build/host-ziggurat', 'build/mips-ziggurat'),
        (['ZIGGURAT=1', 'MATTER=1'], 'build/host-ziggurat-matter', 'build/mips-ziggurat-matter'),
    ):
        result = selected(args)
        assert result.returncode == 0, result.stderr
        value = '1' if 'MATTER=1' in args else '0'
        assert result.stdout.splitlines()[0] == f'{value} {host} {mips}', result.stdout
        assert f'-DMICROHAD_MATTER_ENABLED={value}' in result.stdout
    for args, message in (
        (['MATTER=1'], 'MATTER=1 requires ZIGGURAT=1'),
        (['MATTER=2'], 'MATTER must be 0 or 1'),
        (['MATTER='], 'MATTER must be 0 or 1'),
        (['MATTER=0 1'], 'MATTER must be 0 or 1'),
        (['MATTER=%'], 'MATTER must be 0 or 1'),
        (['ZIGGURAT=1', 'MATTER=yes'], 'MATTER must be 0 or 1'),
        (['ZIGGURAT='], 'ZIGGURAT must be 0 or 1'),
        (['ZIGGURAT=%'], 'ZIGGURAT must be 0 or 1'),
    ):
        result = selected(args)
        assert result.returncode != 0 and message in result.stderr, result.stderr
    # Confirm that an enabled binary cannot bypass its platform gate, even with -j.
    result = subprocess.run(['make', '-n', 'ZIGGURAT=1', 'MATTER=1',
                             'build/mips-ziggurat-matter/microhad-zigbee-native'], cwd=ROOT,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr
    assert 'check-matter-platform.py" mips' in result.stdout
    assert 'controller/platform integration is not implemented' in result.stdout
    print('PASS disabled defaults, isolated enabled outputs, macros, invalid flags, and enabled build gate')


if __name__ == '__main__':
    main()
