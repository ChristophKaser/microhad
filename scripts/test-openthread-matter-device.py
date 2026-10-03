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
    parser.add_argument('--commissioning', action='store_true')
    parser.add_argument('--password-file', type=Path, required=True)
    parser.add_argument('--report', type=Path, default=None)
    parser.add_argument('--ble', action='store_true', help='Commission over synthetic phone GATT/BTP')
    args = parser.parse_args()
    if args.ble and not args.commissioning:
        parser.error('--ble requires --commissioning')
    if args.report is None:
        args.report = ROOT / 'build/rs-matter' / ('openthread-ble-commissioning-device-validation.json' if args.ble else
                                               'openthread-commissioning-device-validation.json'
                                               if args.commissioning else 'openthread-device-validation.json')
    rss_limit = 3072 if args.commissioning else 1536
    limit = 100 if args.commissioning else 22
    marker = 'PASS commissioning complete' if args.commissioning else 'PASS transport complete'
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

    binary = ROOT / 'build/rs-matter' / ('ot-commissioning-mips' if args.commissioning else 'ot-transport-mips')
    relay_binary = ROOT / 'build/openthread-unix/sim-relay-mips'
    payload = binary.read_bytes()
    relay_payload = relay_binary.read_bytes()
    cache_payload = (ROOT / 'build/openthread-unix/drop-test-cache-mips').read_bytes()
    token = uuid.uuid4().hex
    stage = '/tuya/native-test/ot-matter-' + token
    ram = '/tmp/ot-sim-matter-' + token
    before = status()
    if not before.get('online') or not before.get('network_formed'):
        raise RuntimeError('Production must be online and formed before test')
    lines = remote("awk '/^MemFree:/ {print $2}' /proc/meminfo; df -k /tuya | tail -n 1").splitlines()
    free_ram, free_flash = int(lines[0]), int(lines[-1].split()[3])
    if free_ram < 3000 or free_flash * 1024 < len(payload) + len(relay_payload) + len(cache_payload) + 1024 * 1024:
        raise RuntimeError(f'Insufficient resources: RAM={free_ram} KiB flash={free_flash} KiB')
    radio = sim.ThreadRcp(args.host, tcp_nodelay=True)
    try:
        remote('umask 077; mkdir -p /tuya/native-test; mkdir ' + stage + ' && mkdir ' + ram)
        for name, contents in [('probe', payload), ('relay', relay_payload), ('cache-helper', cache_payload)]:
            remote('(set -C; cat > ' + stage + '/' + name + ') && chmod 700 ' + stage + '/' + name, contents)
        # Flush/drop only these newly uploaded files so staging page cache does not
        # masquerade as the running controller's working memory. Never drop global caches.
        remote(stage + '/cache-helper ' + stage + '/probe && ' + stage + '/cache-helper ' + stage + '/relay')
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
{stage}/relay {ram}/radio.sock {radio.address} {radio.port} {limit + 5} &
relay=$!
tries=0
while [ ! -S {ram}/radio.sock ]; do
 tries=$((tries + 1)); [ "$tries" -lt 6 ] || exit 1
 kill -0 "$relay" || exit 1
 sleep 1
done
(ulimit -v 8192; MICROHAD_MATTER_TEST_STATE_DIR={stage}/controller-state exec {stage}/probe 'spinel+hdlc+unix://{ram}/radio.sock?no-reset' {'--ble' if args.ble else ''}) &
child=$!
(sleep {limit}; kill -9 "$child" 2>/dev/null) &
watchdog=$!
while kill -0 "$child" 2>/dev/null; do
 rss=$(awk '/^VmRSS:/ {{print $2}}' /proc/$child/status 2>/dev/null)
 budget=$(awk '/^MemFree:/ {{free=$2}} /^Inactive[(]file[)]:/ {{inactive=$2}}
 /^Dirty:/ {{dirty=$2}} /^Writeback:/ {{writeback=$2}} /^Shmem:/ {{shmem=$2}}
 END {{clean=inactive-dirty-writeback-shmem; if(clean<0)clean=0; print free, free+clean}}' /proc/meminfo)
 set -- $budget
 free=$1
 reclaimable_budget=$2
 ticks=$(awk '{{print $14 + $15}}' /proc/$child/stat 2>/dev/null)
 uptime=$(awk '{{print $1}}' /proc/uptime)
 echo "SAMPLE rss_kib=${{rss:-0}} memfree_kib=$free free_plus_clean_inactive_file_kib=$reclaimable_budget cpu_ticks=${{ticks:-0}} uptime_s=$uptime"
 if [ "${{rss:-0}}" -gt {rss_limit} ] || [ "$free" -lt 512 ] || [ "$reclaimable_budget" -lt 4096 ]; then
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
        result = subprocess.run(ssh + ['sh -c ' + shlex.quote(script)], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=limit + 15)
        output = result.stdout.decode()
        exit_code = result.returncode
        radio_error = radio.error
    finally:
        radio.close()
        remote('rm -rf ' + stage + ' ' + ram)
    after = status()
    report = {'target': args.host, 'radio': 'synthetic RCP', 'real_radio_access': False,
              'linux_ipv6_or_tun': False, 'encrypted_commissioning_tested': args.commissioning, 'synthetic_phone_gatt_tested': args.ble,
                      'real_ble_access': False, 'binary_bytes': len(payload),
              'binary_sha256': hashlib.sha256(payload).hexdigest(),
              'memory_preflight_kib': free_ram,
              'limits': {'rss_kib': rss_limit, 'memfree_floor_kib': 512, 'free_plus_clean_inactive_file_floor_kib': 4096, 'seconds': limit}, 'production_before': before,
              'production_after': after, 'exit_code': exit_code, 'controller_state_reload_tested': 'PASS committed flash fabric reopen' in output,
              'fixture_controller_state_location': 'isolated flash stage',
              'passed': exit_code == 0 and marker in output and not radio_error,
              'synthetic_radio_error': radio_error, 'upload_cache_dropped_for_test_artifacts': True, 'cleanup_completed': True, 'output': output.splitlines()}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise RuntimeError('Device fixture failed; see ' + str(args.report))
    if before != after:
        raise RuntimeError('Production status changed during test')


if __name__ == '__main__':
    main()
