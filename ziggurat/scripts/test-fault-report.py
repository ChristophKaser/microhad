#!/usr/bin/env python3
"""Exercise fatal-signal records without touching services, radio or saved state."""
import argparse
import os
from pathlib import Path
import re
import secrets
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts'))
from toolchain_config import settings
from workspace_config import setting

FIXTURE = r'''
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
int main(int argc, char **argv) {
    struct rlimit limit = {0, 0};
    void *page;
    volatile unsigned char value;
    if (setrlimit(RLIMIT_CORE, &limit) != 0 || argc != 2) return 2;
    if (atoi(argv[1]) == SIGILL) { raise(SIGILL); return 3; }
    page = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 4;
    printf("expected_addr=0x%lx\n", (unsigned long)page);
    fflush(stdout);
    value = *(volatile unsigned char *)page;
    (void)value;
    return 5;
}
'''


def check(result, number, remote=False):
    expected_return = 128 + number if remote else -number
    assert result.returncode == expected_return, (result.returncode, result.stderr)
    name = 'SIGSEGV' if number == signal.SIGSEGV else 'SIGILL'
    match = re.search(name + r' pc=0x([0-9a-f]+) ra=0x([0-9a-f]+) sp=0x([0-9a-f]+) addr=0x([0-9a-f]+) code=0x([0-9a-f]+) tid=0x([0-9a-f]+)', result.stderr)
    assert match, result.stderr
    assert int(match[1], 16) and int(match[3], 16) and int(match[6], 16), match[0]
    if number == signal.SIGSEGV:
        address = re.search(r'expected_addr=0x([0-9a-f]+)', result.stdout)
        assert address and int(address[1], 16) == int(match[4], 16), (result.stdout, match[0])
    print('PASS', 'isolated MIPS' if remote else 'host', name, match[0], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', action='store_true', help='Run an isolated /tmp fixture on the configured gateway')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fault-report-') as temporary:
        root = Path(temporary)
        source = root/'probe.c'; source.write_text(FIXTURE)
        binary = root/'fault-probe'
        compiler = settings()['TARGET_CC'] if args.device else 'cc'
        flags = ['-static', '-msoft-float'] if args.device else []
        subprocess.run([str(compiler), '-O0', '-g', '-std=gnu99', '-Wall', '-Wextra', '-Werror', *flags,
                        str(source), str(ROOT/'ziggurat/stock-uclibc-compat/native/fault_report.c'),
                        '-o', str(binary)], check=True)
        if not args.device:
            for number in (signal.SIGSEGV, signal.SIGILL):
                check(subprocess.run([str(binary), str(number)], capture_output=True, text=True, timeout=5), number)
            return
        ssh = ['bash', str(ROOT/'scripts/workspace-ssh.sh'), '--', setting('GATEWAY_IP')]
        remote = '/tmp/matter-fault-probe.' + secrets.token_hex(8)
        subprocess.run(ssh + [f'umask 077; mkdir {shlex.quote(remote)}'], check=True)
        assert re.fullmatch(r'/tmp/matter-fault-probe\.[A-Za-z0-9]+', remote), remote
        try:
            with binary.open('rb') as stream:
                subprocess.run(ssh + [f'cat > {shlex.quote(remote)}/fault-probe; chmod 700 {shlex.quote(remote)}/fault-probe'], stdin=stream, check=True)
            for number in (signal.SIGSEGV, signal.SIGILL):
                result = subprocess.run(ssh + [f'cd {shlex.quote(remote)}; ulimit -c 0; ./fault-probe {number}'], capture_output=True, text=True, timeout=15)
                check(result, number, remote=True)
        finally:
            subprocess.run(ssh + [f'rm -rf {shlex.quote(remote)}'], check=True)


if __name__ == '__main__':
    main()
