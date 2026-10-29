#!/usr/bin/env python3
"""Explicit isolated device crash test; restore kernel settings even on failure."""
import argparse
import json
from pathlib import Path
import secrets
import shlex
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='Explicit gateway IPv4 address')
    parser.add_argument('--receiver', required=True, help='Host receiver IPv4 address')
    parser.add_argument('--port', type=int, default=9999)
    parser.add_argument('--transport-only', action='store_true', help='Test the MIPS TCP helper without crashing')
    parser.add_argument('--output', type=Path, default=ROOT / 'diagnostics/core-dumps')
    args = parser.parse_args()
    config = settings()
    binary = ROOT / 'build' / config['TARGET_BUILD_SUBDIR'] / 'gateway-core'
    ssh = ['bash', str(ROOT / 'scripts/workspace-ssh.sh'), '--', args.host]
    remote = '/tmp/gateway-core-probe.' + secrets.token_hex(8)
    def run(command, **kwargs):
        return subprocess.run(ssh + [command], check=True, **kwargs)
    def upload(local, destination):
        with local.open('rb') as stream:
            run(f'cat > {shlex.quote(destination)}', stdin=stream)
    before = set(args.output.glob('*/capture.json'))
    with tempfile.TemporaryDirectory(prefix='core-probe-') as temporary:
        source = Path(temporary) / 'probe.c'
        fixture = Path(temporary) / 'core-probe'
        source.write_text('#include <sys/mman.h>\n#include <sys/prctl.h>\n#include <stdio.h>\nint main(void) {\n'
                          'printf("dumpable=%d\\n",prctl(PR_GET_DUMPABLE)); fflush(stdout);\n'
                          'volatile char *p=mmap(0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);\n'
                          'if(p==(void *)-1) return 2; return *p; }\n')
        subprocess.run([str(config['TARGET_CC']), '-O0', '-g', '-static', '-msoft-float',
                        str(source), str(ROOT / 'ziggurat/stock-uclibc-compat/native/fault_report.c'),
                        '-o', str(fixture)], check=True)
        run(f'umask 077; mkdir {remote}')
        try:
            upload(binary, remote + '/gateway-core')
            upload(fixture, remote + '/core-probe')
            destination = shlex.quote(f'{args.receiver} {args.port}')
            run(f"chmod 700 {remote}/gateway-core {remote}/core-probe; printf '%s\\n' {destination} > {remote}/core-capture.conf")
            if args.transport_only:
                payload = b'gateway core transport probe\n' * 80000
                run(f'{remote}/gateway-core $$ 0 0', input=payload, timeout=45)
                matches = set(args.output.glob('*/capture.json')) - before
                for path in matches:
                    if (path.parent / 'core').exists() and (path.parent / 'core').read_bytes() == payload:
                        print(f'PASS MIPS TCP stream: {len(payload)} bytes; larger than RAM fallback')
                        return
                raise RuntimeError('Transport payload not received intact')
            # One SSH shell owns restoration even if the fixture fails.
            run(f'''set -eu
old_pattern=$(cat /proc/sys/kernel/core_pattern)
old_limit=$(cat /proc/sys/kernel/core_pipe_limit)
restore() {{ printf '%s\\n' "$old_pattern" > /proc/sys/kernel/core_pattern; printf '%s\\n' "$old_limit" > /proc/sys/kernel/core_pipe_limit; }}
trap restore EXIT HUP INT TERM
printf '1\\n' > /proc/sys/kernel/core_pipe_limit
printf '%s\\n' '|{remote}/gateway-core %p %s %t' > /proc/sys/kernel/core_pattern
ulimit -c unlimited
printf 'transport probe' | {remote}/gateway-core $$ 0 0
{remote}/core-probe || [ "$?" = 139 ]
''', timeout=45)
            for _ in range(100):
                matches = set(args.output.glob('*/capture.json')) - before
                for path in matches:
                    data = json.loads(path.read_text())
                    process = path.parent / 'process.txt'
                    if process.exists() and (remote + '/core-probe') in process.read_text():
                        assert data.get('complete'), data
                        core = path.parent / 'core'
                        assert core.read_bytes()[:4] == b'\x7fELF'
                        artifact = path.parent / 'core-probe.elf'
                        artifact.write_bytes(fixture.read_bytes())
                        print(f'PASS isolated MIPS core: {core} ({core.stat().st_size} bytes)')
                        return
                time.sleep(.1)
            raise RuntimeError('No complete probe core received')
        finally:
            run(f'rm -rf {remote}')


if __name__ == '__main__':
    main()
