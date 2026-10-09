#!/usr/bin/env python3
"""Run bounded probes only in a freshly allocated gateway /tmp directory."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--password-file', required=True, type=Path)
    parser.add_argument('--host', default='192.168.178.62')
    parser.add_argument('--backend', required=True, choices=['rsdk', 'crosstool-ng'])
    args = parser.parse_args()
    destination = ROOT / 'build/toolchain-proof/probes' / args.backend
    if not (destination / 'report.json').exists():
        raise SystemExit('Build and local verification must pass first.')
    report = json.loads((destination / 'report.json').read_text())
    if report['backend'] != args.backend:
        raise SystemExit('Build report names a different backend.')
    for name in ['native', 'rust']:
        if hashlib.sha256((destination / name).read_bytes()).hexdigest() != report[name]['sha256']:
            raise SystemExit('Binary changed since verification: ' + name)
    ssh = ['sshpass', '-f', str(args.password_file.resolve()), 'ssh', '-T', '-o', 'ConnectTimeout=8',
           '-o', 'StrictHostKeyChecking=yes', '-o', 'HostKeyAlgorithms=+ssh-rsa',
           '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa', 'root@' + args.host]
    def remote(command, **kwargs):
        result = subprocess.run([*ssh, command], timeout=75, **kwargs)
        if result.returncode and kwargs.get('capture_output'):
            print(result.stderr, end='', flush=True)
        result.check_returncode()
        return result
    kernel = remote('cat /proc/version', capture_output=True, text=True).stdout.strip()
    (destination / 'kernel.device.log').write_text(kernel + '\n')
    print('Gateway kernel:', kernel, flush=True)
    directory = '/tmp/compiler-proof.' + secrets.token_hex(12)
    remote('umask 077; mkdir ' + shlex.quote(directory), capture_output=True, text=True)
    if not re.fullmatch(r'/tmp/compiler-proof\.[A-Za-z0-9]+', directory):
        raise SystemExit('Unexpected remote temporary directory: ' + repr(directory))
    remote_path = shlex.quote(directory)
    try:
        for name in ['native', 'rust']:
            path = remote_path + '/' + name
            with (destination / name).open('rb') as stream:
                remote('cat > ' + path + ' && chmod 700 ' + path, stdin=stream)
            # Watchdog avoids depending on the stock BusyBox timeout applet.
            command = ('ulimit -c 0; ' + path + ' & proof_pid=$!; '
                       '(sleep 60 & timer_pid=$!; trap \'kill "$timer_pid" 2>/dev/null; exit 0\' TERM; '
                       'wait "$timer_pid"; kill -KILL "$proof_pid" 2>/dev/null) </dev/null >/dev/null 2>&1 & watchdog_pid=$!; '
                       'wait "$proof_pid"; result=$?; kill "$watchdog_pid" 2>/dev/null; '
                       'wait "$watchdog_pid" 2>/dev/null; exit "$result"')
            started = time.monotonic()
            result = subprocess.run([*ssh, command], capture_output=True, text=True, timeout=75)
            log = result.stdout + result.stderr + '\nexit=' + str(result.returncode) + '\nelapsed=' + str(time.monotonic() - started) + '\n'
            (destination / (name + '.device.log')).write_text(log)
            print(log, end='', flush=True)
            result.check_returncode()
            if 'PASS ' + ('native' if name == 'native' else 'Rust') + ' compiler proof complete' not in result.stdout:
                raise SystemExit('Probe exited without its completion checkpoint.')
    finally:
        remote('rm -rf ' + remote_path, stdin=subprocess.DEVNULL)
    print('PASS gateway proof:', args.backend)


if __name__ == '__main__':
    main()
