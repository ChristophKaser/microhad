#!/usr/bin/env python3
"""Stage the TCP core helper on an explicitly selected gateway and try activation."""
import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--receiver', required=True)
    parser.add_argument('--stage-only', action='store_true', help='Install files without invoking activation')
    parser.add_argument('--port', type=int, default=9999)
    args = parser.parse_args()
    import ipaddress
    ipaddress.IPv4Address(args.receiver)
    if not 1 <= args.port <= 65535:
        parser.error('port must be 1..65535')
    ssh = ['bash', str(ROOT / 'scripts/workspace-ssh.sh'), '--', args.host]
    def run(command, **kwargs):
        return subprocess.run(ssh + [command], check=True, **kwargs)
    binary = ROOT / 'build' / settings()['TARGET_BUILD_SUBDIR'] / 'gateway-core'
    files = [('gateway-core', binary.read_bytes(), '700'),
             ('core-capture-control.sh', (ROOT / 'scripts/core-capture/control.sh').read_bytes(), '700'),
             ('core-capture.conf', f'{args.receiver} {args.port}\n'.encode(), '600')]
    needed = sum(len(data) for _, data, _ in files) * 2 // 1024 + 512
    run("test -d /tuya/native && df -k /tuya | awk 'NR==2 {exit !($4 >= " + str(needed) + ")}'")
    for name, data, mode in files:
        destination = '/tuya/native/' + name
        temporary = destination + '.core-stage'
        digest = hashlib.md5(data).hexdigest()
        command = (f'umask 077; test ! -L {temporary} && cat > {temporary} && '
                   f'test "$(md5sum {temporary} | awk \'{{print $1}}\')" = {shlex.quote(digest)} && '
                   f'chmod {mode} {temporary} && mv {temporary} {destination}')
        run(command, input=data)
    if args.stage_only:
        print('Helper staged; activation not requested.')
        return
    # A known unsupported kernel is a staged deployment, not an activation success.
    result = subprocess.run(ssh + ['/tuya/native/core-capture-control.sh start'])
    if result.returncode:
        print('Helper staged; capture could not activate. Existing kernel settings preserved.')
        raise SystemExit(3)
    run("test \"$(cat /proc/sys/kernel/core_pattern)\" = '|/tuya/native/gateway-core %p %s %t' && cat /proc/sys/kernel/core_pattern && cat /proc/sys/kernel/core_pipe_limit")


if __name__ == '__main__':
    main()
