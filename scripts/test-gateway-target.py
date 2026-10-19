#!/usr/bin/env python3
"""Verify actual ELF rejection and instruction decoding using synthetic binaries."""
import os
from pathlib import Path
import subprocess
import tempfile
from toolchain_config import ROOT, settings


def main():
    values = settings()
    verifier = ROOT / 'scripts/verify-gateway-target.py'
    host_env = dict(os.environ, TARGET_READELF=values['HOST_READELF'])
    result = subprocess.run(['python3', verifier, '/bin/true'], env=host_env, capture_output=True, text=True)
    assert result.returncode != 0 and 'expected ELF32 big-endian MIPS-I/O32' in result.stderr
    with tempfile.TemporaryDirectory() as temporary:
        source = Path(temporary) / 'unsupported.c'
        binary = Path(temporary) / 'unsupported'
        # Emit raw encodings so the ELF remains marked MIPS-I. A standard MIPS32r2
        # decoder must still recognize the instructions rejected by the gateway.
        source.write_text('int main(void) { __asm__ __volatile__(".word 0x7c02e83b\\n.word 0x00000034"); return 0; }\n')
        subprocess.run([values['TARGET_CC'], '-static', '-msoft-float', source, '-o', binary], check=True)
        result = subprocess.run(['python3', verifier, binary], capture_output=True, text=True)
        assert result.returncode != 0 and 'unsupported stock-kernel instructions' in result.stderr, result.stderr
        assert "'rdhwr': 1" in result.stderr and "'teq': 1" in result.stderr, result.stderr
        source.write_text('#include <stdio.h>\nint main(void) { puts("linkage fixture"); return 0; }\n')
        for mode in ('static', 'dynamic'):
            flags = ['-static'] if mode == 'static' else []
            subprocess.run([values['TARGET_CC'], *flags, '-msoft-float', source, '-o', binary], check=True)
            result = subprocess.run(['python3', verifier, '--link-mode', mode, binary], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            other = 'dynamic' if mode == 'static' else 'static'
            result = subprocess.run(['python3', verifier, '--link-mode', other, binary], capture_output=True, text=True)
            assert result.returncode != 0, 'verifier accepted wrong linkage mode'
        subprocess.run([values['TARGET_CC'], '-fPIC', '-pie', '-msoft-float', source, '-o', binary], check=True)
        result = subprocess.run(['python3', verifier, '--link-mode', 'dynamic', binary], capture_output=True, text=True)
        assert result.returncode != 0 and 'expected non-PIE ET_EXEC' in result.stderr, result.stderr
        subprocess.run([values['TARGET_CC'], '-msoft-float', '-Wl,--dynamic-linker=/wrong/loader', source, '-o', binary], check=True)
        result = subprocess.run(['python3', verifier, '--link-mode', 'dynamic', binary], capture_output=True, text=True)
        assert result.returncode != 0 and 'expected /lib/ld-uClibc.so.0' in result.stderr, result.stderr
        library = Path(temporary) / 'libgateway_missing_fixture.so'
        provider = Path(temporary) / 'provider.c'
        provider.write_text('int fixture(void) { return 0; }\n')
        source.write_text('int fixture(void); int main(void) { return fixture(); }\n')
        subprocess.run([values['TARGET_CC'], '-shared', '-fPIC', provider, '-o', library], check=True)
        subprocess.run([values['TARGET_CC'], source, '-L' + temporary, '-lgateway_missing_fixture', '-o', binary], check=True)
        result = subprocess.run(['python3', verifier, '--link-mode', 'dynamic', binary], capture_output=True, text=True)
        assert result.returncode != 0 and 'shared library not provided by stock image' in result.stderr, result.stderr
    print('PASS verifier rejects host ELF, forbidden instructions, wrong linkage, PIE, wrong loader and missing runtime; static and dynamic fixtures pass')


if __name__ == '__main__':
    main()
