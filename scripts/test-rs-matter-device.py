#!/usr/bin/env python3
"""Bounded crypto-only probe; no UART, sockets, fabric or production changes."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--password-file', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/rs-matter/probe-mips')
    parser.add_argument('--report', type=Path, default=ROOT / 'build/rs-matter/device-validation.json')
    args = parser.parse_args()
    ssh = ['sshpass', '-f', str(args.password_file), 'ssh', '-o', 'ConnectTimeout=8',
           '-o', 'StrictHostKeyChecking=yes', '-o', 'HostKeyAlgorithms=+ssh-rsa',
           '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa', '-o', 'PreferredAuthentications=password',
           '-o', 'PubkeyAuthentication=no', 'root@' + args.host]

    def remote(command, payload=None, timeout=15):
        return subprocess.run(ssh + [command], input=payload, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=True, timeout=timeout).stdout.decode()

    def status():
        with urllib.request.urlopen('http://' + args.host + '/api/status', timeout=8) as response:
            data = json.load(response)
        return {key: data[key] for key in ('online', 'network_formed', 'coordinator_backend') if key in data}

    payload = args.binary.read_bytes()
    stage = '/tuya/native-test/rs-matter-probe-' + uuid.uuid4().hex
    before = status()
    if not before.get('online') or not before.get('network_formed'):
        raise RuntimeError('Production must be online and formed before test')
    preflight = remote("awk '/^MemFree:/ {print $2}' /proc/meminfo; df -k /tuya | tail -n 1")
    lines = preflight.splitlines()
    free_ram = int(lines[0])
    free_flash = int(lines[-1].split()[3])
    if free_ram < 3072 or free_flash * 1024 < len(payload) + 1024 * 1024:
        raise RuntimeError(f'Insufficient RAM/flash: MemFree={free_ram} KiB, flash_free={free_flash} KiB')
    try:
        remote('mkdir -p /tuya/native-test && (set -C; cat > ' + shlex.quote(stage) +
               ') && chmod 700 ' + shlex.quote(stage), payload)
        # Samples use only public resource counters. The watchdog bounds this crypto-only process.
        script = '''
ulimit -c 0
ulimit -v 4096
child=
watchdog=
trap '[ -z "$child" ] || kill "$child" 2>/dev/null; [ -z "$watchdog" ] || kill "$watchdog" 2>/dev/null' EXIT HUP INT TERM
BIN &
child=$!
(sleep 40; kill "$child" 2>/dev/null) &
watchdog=$!
while kill -0 "$child" 2>/dev/null; do
    awk '/^VmRSS:/ {print "SAMPLE rss_kib=" $2}' /proc/$child/status 2>/dev/null
    free=$(awk '/^MemFree:/ {print $2}' /proc/meminfo)
    echo "SAMPLE memfree_kib=$free"
    if [ "$free" -lt 1024 ]; then kill "$child" 2>/dev/null; break; fi
    sleep 1
done
wait "$child"
result=$?
child=
kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null
watchdog=
echo "EXIT $result"
exit "$result"
'''.replace('BIN', shlex.quote(stage))
        output = remote('sh -c ' + shlex.quote(script), timeout=55)
        if 'PASS complete elapsed_ms=' not in output:
            raise RuntimeError('Probe did not complete all checks')
    finally:
        remote('rm -f ' + shlex.quote(stage))
    after = status()
    report = {'target': args.host, 'binary_bytes': len(payload),
              'binary_sha256': hashlib.sha256(payload).hexdigest(),
              'memory_preflight_kib': free_ram, 'production_before': before,
              'production_after': after, 'output': output.splitlines(),
              'radio_access': False, 'network_access': False, 'cleanup_completed': True}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if before != after:
        raise RuntimeError('Production status changed during probe')


if __name__ == '__main__':
    main()
