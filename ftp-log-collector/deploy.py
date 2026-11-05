#!/usr/bin/env python3
"""Initial install of the collector on an explicitly named paired gateway."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    ssh = ['bash', str(ROOT / 'scripts/workspace-ssh.sh'), '--', args.host]

    def run(command, **kwargs):
        return subprocess.run(ssh + [command], check=True, **kwargs)

    binary = ROOT / 'build' / settings()['TARGET_BUILD_SUBDIR'] / 'ftp-log-collector'
    artifact = binary.read_bytes()
    # This initial-install flow deliberately refuses to overwrite an existing
    # collector or its private settings; update needs a separate guarded flow.
    needed = (len(artifact) + 1023) // 1024 + 512 + 64
    run("test -d /tuya/native && test ! -e /tuya/native/ftp-log-collector && "
        "test ! -e /tuya/native/ftp-log-collector.conf && "
        "test ! -e /tuya/native/.ftp-collector-stage && "
        "df -k /tuya | awk 'END {exit !($4 >= " + str(needed) + ")}'")
    raw = run('cat /tuya/native/fritz-boxes.tsv', stdout=subprocess.PIPE).stdout.decode('utf-8')
    rows = [line.split('\t') for line in raw.splitlines() if line]
    boxes = {tuple(unquote(value) for value in row[2:5]) for row in rows if len(row) == 7}
    if len(boxes) != 1 or any(len(row) != 7 for row in rows):
        raise ValueError('Expected exactly one configured FRITZ!Box')
    ftp_host, ftp_user, ftp_password = next(iter(boxes))
    if any(not value or '\n' in value or '\r' in value for value in (ftp_host, ftp_user, ftp_password)):
        raise ValueError('FRITZ!Box settings cannot be represented safely')
    config = (f'ftp_host={ftp_host}\nftp_user={ftp_user}\nftp_password={ftp_password}\n'
              'ftp_directory=/\nlog_file_bytes=10485760\n').encode()
    old_json = run('cat /tuya/native/config.json', stdout=subprocess.PIPE).stdout

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate key in gateway configuration')
            result[key] = value
        return result

    data = json.loads(old_json, object_pairs_hook=unique)
    if not isinstance(data, dict):
        raise ValueError('Gateway configuration is not an object')
    data['syslog_remote'] = '127.0.0.1:5514'
    native = (json.dumps(data, ensure_ascii=True, indent=2) + '\n').encode()
    core = b'127.0.0.1 9999\n'
    digest = hashlib.md5(old_json).hexdigest()
    stage = '/tuya/native/.ftp-collector-stage'
    run(f'umask 077; mkdir {stage}')
    try:
        for name, content, mode in [('collector', artifact, '700'), ('config', config, '600'),
                                    ('native', native, '600'), ('core', core, '600')]:
            expected = hashlib.md5(content).hexdigest()
            path = f'{stage}/{name}'
            run(f'umask 077; cat > {path} && chmod {mode} {path} && '
                f'test "$(md5sum {path} | awk \'{{print $1}}\')" = {expected}', input=content)
        run(f'{stage}/collector --check-config {stage}/config && '
            f'/tuya/native/microhad --print-config syslog_remote --config {stage}/native >/dev/null && '
            f'test "$(md5sum /tuya/native/config.json | awk \'{{print $1}}\')" = {digest} && '
            'available=$(df -k /tuya | awk \'END {print $4}\'); '
            'test "$available" -ge 512')
        run(f'test ! -e /tuya/native/ftp-log-collector && test ! -e /tuya/native/ftp-log-collector.conf && '
            f'mv {stage}/collector /tuya/native/ftp-log-collector && '
            f'mv {stage}/config /tuya/native/ftp-log-collector.conf && '
            f'mv {stage}/native /tuya/native/config.json && '
            f'mv {stage}/core /tuya/native/core-capture.conf')
    finally:
        run(f'rm -rf {stage}', stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print('Collector and destinations staged; deploying the paired supervisor.', flush=True)
    subprocess.run([str(ROOT / 'deploy.sh'), '--host', args.host, '--services', 'supervisor',
                    '--target', '/tuya/native'], cwd=ROOT, check=True)
    run('test -x /tuya/native/ftp-log-collector && '
        '/tuya/native/ftp-log-collector --check-config /tuya/native/ftp-log-collector.conf && '
        '/bin/busybox ps | grep "[f]tp-log-collector" >/dev/null')
    print('FTP log collector is running.')


if __name__ == '__main__':
    main()
