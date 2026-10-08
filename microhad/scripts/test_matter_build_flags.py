#!/usr/bin/env python3
"""Check Make feature selection without compiling, fetching, or touching a device."""
from pathlib import Path
import subprocess
import hashlib
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def selected(arguments):
    # A private additional makefile target prints the selected public build contract.
    rule = '''.PHONY: print-matter-selection
print-matter-selection:
\t@echo $(MATTER) $(HOST_BUILD) $(MIPS_BUILD)
\t@echo $(HOST_CXXFLAGS)
'''
    return subprocess.run(['make', '--no-print-directory', '-f', 'Makefile', '-f', '-',
                           *arguments, 'print-matter-selection'], cwd=ROOT, input=rule,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    for args, host, mips in (
        ([], 'build/host-ziggurat-matter', 'build/mips-ziggurat-matter'),
        (['ZIGGURAT=0'], 'build/host', 'build/mips'),
        (['ZIGGURAT=0', 'MATTER=0'], 'build/host', 'build/mips'),
        (['ZIGGURAT=1', 'MATTER=0'], 'build/host-ziggurat', 'build/mips-ziggurat'),
        (['ZIGGURAT=1', 'MATTER=1'], 'build/host-ziggurat-matter', 'build/mips-ziggurat-matter'),
    ):
        result = selected(args)
        assert result.returncode == 0, result.stderr
        value = '0' if 'MATTER=0' in args or 'ZIGGURAT=0' in args else '1'
        assert result.stdout.splitlines()[0] == f'{value} {host} {mips}', result.stdout
        assert f'-DMICROHAD_MATTER_ENABLED={value}' in result.stdout
    for args, message in (
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
                             'build/mips-ziggurat-matter/microhad'], cwd=ROOT,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr
    assert 'Matter native IPC, commissioning, Thread, and device-control APIs enabled' in result.stdout
    result = subprocess.run(['make', '-B', '-n', 'ZIGGURAT=1', 'MATTER=1', 'mips'], cwd=ROOT,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr
    assert 'build/mips-ziggurat-matter/microhad.matter-manifest' in result.stdout
    assert "'matter=1'" in result.stdout and 'sha256sum' in result.stdout
    with tempfile.TemporaryDirectory(prefix='matter-build-manifest-') as directory:
        binary = Path(directory) / 'microhad'
        binary.write_bytes(b'synthetic Matter-enabled MIPS executable')
        manifest = Path(str(binary) + '.matter-manifest')
        result = subprocess.run(['make', '--no-print-directory', 'ZIGGURAT=1', 'MATTER=1',
                                 f'MIPS_BIN={binary}', str(manifest)], cwd=ROOT,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.returncode == 0, result.stderr
        assert manifest.read_text().splitlines() == [
            'format=microhad-build-v1', 'ziggurat=1', 'matter=1',
            f'binary_sha256={hashlib.sha256(binary.read_bytes()).hexdigest()}']
    print('PASS Matter defaults, legacy overrides, isolated outputs, feature macros/manifest, invalid flags, and build gate')


if __name__ == '__main__':
    main()
