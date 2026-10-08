#!/usr/bin/env python3
"""Run native IPC framing vectors on host and optionally the factory gateway; no radio or state access."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'scripts'))
from toolchain_config import settings

TOOLCHAIN = settings()
RSDK = TOOLCHAIN['RSDK_ROOT']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host')
    parser.add_argument('--password-file', type=Path)
    args = parser.parse_args()
    if bool(args.host) != bool(args.password_file):
        parser.error('--host and --password-file must be provided together')
    report = {'host': False, 'factory_mips': False, 'real_radio_access': False}
    with tempfile.TemporaryDirectory(prefix='matter-ipc-codec-') as directory:
        directory = Path(directory)
        driver = directory / 'driver.cpp'
        driver.write_text('#include <cstdio>\nvoid runMatterProtocolUnitTests();\n'
                          'int main() { runMatterProtocolUnitTests(); puts("PASS Matter IPC codec"); return 0; }\n')
        sources = [str(driver), str(ROOT / 'src/matter_protocol.cpp'), str(ROOT / 'test/unit_matter_protocol.cpp')]
        host = directory / 'host'
        subprocess.run(['g++', '-std=gnu++03', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
                        '-I' + str(ROOT / 'include'), *sources, '-o', str(host)], check=True)
        subprocess.run([str(host)], check=True)
        report['host'] = True
        if args.host:
            target = directory / 'mips'
            subprocess.run([str(TOOLCHAIN['TARGET_CXX']), '-Wall', '-Wextra', '-Werror', '-pedantic',
                            '-Os', '-msoft-float', '-static', '-I' + str(ROOT / 'include'), *sources,
                            '-o', str(target)], check=True)
            subprocess.run([str(TOOLCHAIN['TARGET_STRIP']), str(target)], check=True)
            ssh = ['sshpass', '-f', str(args.password_file), 'ssh', '-o', 'ConnectTimeout=8',
                   '-o', 'StrictHostKeyChecking=yes', '-o', 'HostKeyAlgorithms=+ssh-rsa',
                   '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa', '-o', 'PreferredAuthentications=password',
                   '-o', 'PubkeyAuthentication=no', 'root@' + args.host]
            stage = '/tuya/native-test/matter-ipc-' + uuid.uuid4().hex
            def remote(command, payload=None):
                return subprocess.run(ssh + [command], input=payload, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, check=True, timeout=15).stdout.decode()
            try:
                payload = target.read_bytes()
                remote('umask 077; mkdir -p /tuya/native-test; (set -C; cat > ' + stage +
                       ') && chmod 700 ' + stage, payload)
                command = f'''ulimit -c 0; ulimit -v 4096
{stage} &
child=$!
(sleep 8; kill -9 "$child" 2>/dev/null) &
watchdog=$!
trap 'kill "$child" "$watchdog" 2>/dev/null' EXIT HUP INT TERM
wait "$child"
exit $?
'''
                output = remote('sh -c ' + shlex.quote(command))
                if 'PASS Matter IPC codec' not in output:
                    raise RuntimeError('Factory codec probe did not pass')
                report.update(factory_mips=True, binary_bytes=len(payload))
            finally:
                remote('rm -f ' + stage)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
