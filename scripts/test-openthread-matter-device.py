#!/usr/bin/env python3
"""Bounded factory gateway UDP test with synthetic radio; never accesses the UART."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ot_sim', ROOT / 'scripts/test-openthread-device.py')
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--password-file', type=Path, required=True)
    parser.add_argument('--report', type=Path, default=ROOT / 'build/rs-matter/openthread-device-validation.json')
    args = parser.parse_args()
    ssh = ['sshpass', '-f', str(args.password_file), 'ssh', '-o', 'ConnectTimeout=8',
           '-o', 'StrictHostKeyChecking=yes', '-o', 'HostKeyAlgorithms=+ssh-rsa',
           '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa', '-o', 'PreferredAuthentications=password',
           '-o', 'PubkeyAuthentication=no', 'root@' + args.host]

    def remote(command, payload=None, timeout=15):
        return subprocess.run(ssh + [command], input=payload, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=True, timeout=timeout).stdout.decode()

    def status():
        with urllib.request.urlopen('http://' + args.host + '/api/status', timeout=8) as response:
            data = json.load(response)
        return {key: data[key] for key in ('online', 'network_formed', 'coordinator_backend') if key in data}

    binary = ROOT / 'build/rs-matter/ot-transport-mips'
    relay_binary = ROOT / 'build/openthread-unix/sim-relay-mips'
    payload = binary.read_bytes()
    relay_payload = relay_binary.read_bytes()
    token = uuid.uuid4().hex
    stage = '/tuya/native-test/ot-matter-' + token
    ram = '/tmp/ot-sim-matter-' + token
    before = status()
    if not before.get('online') or not before.get('network_formed'):
        raise RuntimeError('Production must be online and formed before test')
    lines = remote("awk '/^MemFree:/ {print $2}' /proc/meminfo; df -k /tuya | tail -n 1").splitlines()
    free_ram, free_flash = int(lines[0]), int(lines[-1].split()[3])
    if free_ram < 3000 or free_flash * 1024 < len(payload) + len(relay_payload) + 1024 * 1024:
        raise RuntimeError(f'Insufficient resources: RAM={free_ram} KiB flash={free_flash} KiB')
    radio = sim.ThreadRcp(args.host, tcp_nodelay=True)
    try:
        remote('umask 077; mkdir -p /tuya/native-test; mkdir ' + stage + ' && mkdir ' + ram)
        for name, contents in [('probe', payload), ('relay', relay_payload)]:
            remote('(set -C; cat > ' + stage + '/' + name + ') && chmod 700 ' + stage + '/' + name, contents)
        script = f'''
ulimit -c 0
cd {ram} || exit 1
child=
relay=
watchdog=
cleanup() {{
 [ -z "$child" ] || kill "$child" 2>/dev/null
 [ -z "$relay" ] || kill "$relay" 2>/dev/null
 [ -z "$watchdog" ] || kill "$watchdog" 2>/dev/null
}}
trap cleanup EXIT HUP INT TERM
{stage}/relay {ram}/radio.sock {radio.address} {radio.port} 25 &
relay=$!
tries=0
while [ ! -S {ram}/radio.sock ]; do
 tries=$((tries + 1)); [ "$tries" -lt 6 ] || exit 1
 kill -0 "$relay" || exit 1
 sleep 1
done
(ulimit -v 8192; exec {stage}/probe 'spinel+hdlc+unix://{ram}/radio.sock?no-reset') &
child=$!
(sleep 22; kill -9 "$child" 2>/dev/null) &
watchdog=$!
while kill -0 "$child" 2>/dev/null; do
 rss=$(awk '/^VmRSS:/ {{print $2}}' /proc/$child/status 2>/dev/null)
 free=$(awk '/^MemFree:/ {{print $2}}' /proc/meminfo)
 echo "SAMPLE rss_kib=${{rss:-0}} memfree_kib=$free"
 if [ "${{rss:-0}}" -gt 1536 ] || [ "$free" -lt 1024 ]; then
  echo TEST_LIMIT_REACHED
  kill -9 "$child" 2>/dev/null
  break
 fi
 sleep 1
done
wait "$child"
result=$?
child=
echo "EXIT $result"
exit "$result"
'''
        try:
            output = remote('sh -c ' + shlex.quote(script), timeout=35)
        except subprocess.CalledProcessError as error:
            raise RuntimeError(error.stdout.decode()) from error
        if 'PASS transport complete' not in output or radio.error:
            raise RuntimeError('Probe failed: ' + output + '\nRCP: ' + str(radio.error))
    finally:
        radio.close()
        remote('rm -rf ' + stage + ' ' + ram)
    after = status()
    report = {'target': args.host, 'radio': 'synthetic RCP', 'real_radio_access': False,
              'linux_ipv6_or_tun': False, 'binary_bytes': len(payload),
              'binary_sha256': hashlib.sha256(payload).hexdigest(),
              'memory_preflight_kib': free_ram, 'production_before': before,
              'production_after': after, 'cleanup_completed': True, 'output': output.splitlines()}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if before != after:
        raise RuntimeError('Production status changed during test')


if __name__ == '__main__':
    main()
