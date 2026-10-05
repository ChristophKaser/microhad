#!/usr/bin/env python3
"""Bounded factory gateway UDP test with synthetic radio; never accesses the UART."""
import argparse
import hashlib
import importlib.util
import json
import re
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
    parser.add_argument('--service-owner', action='store_true',
                        help='Exercise production owned commissioning and device manager')
    parser.add_argument('--service-discovery', action='store_true',
                        help='Exercise production SRP registration and DNS-SD discovery')
    parser.add_argument('--sensor-reports', action='store_true',
                        help='Subscribe to and publish all synthetic CASE sensor reports')
    parser.add_argument('--icd', action='store_true',
                        help='Exercise LIT registration, authenticated Check-In and wake-window dispatch')
    args = parser.parse_args()
    if args.ble and not args.commissioning:
        parser.error('--ble requires --commissioning')
    if args.service_owner and not (args.commissioning and args.ble):
        parser.error('--service-owner requires --commissioning --ble')
    if args.service_discovery and not args.service_owner:
        parser.error('--service-discovery requires --service-owner')
    if args.sensor_reports and not args.service_owner:
        parser.error('--sensor-reports requires --service-owner')
    if args.icd and not args.service_owner:
        parser.error('--icd requires --service-owner')
    if args.report is None:
        args.report = ROOT / 'build/rs-matter' / ('openthread-ble-commissioning-device-validation.json' if args.ble else
                                               'openthread-commissioning-device-validation.json'
                                               if args.commissioning else 'openthread-device-validation.json')
    rss_limit = 3072 if args.commissioning else 1536
    limit = 160 if args.icd else 100 if args.commissioning else 22
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
    if free_ram < 3000 or free_flash * 1024 < len(payload) + 900 * 1024:
        raise RuntimeError(f'Insufficient resources: RAM={free_ram} KiB flash={free_flash} KiB')
    radio = sim.ThreadRcp(args.host, tcp_nodelay=True)
    try:
        remote('umask 077; mkdir -p /tuya/native-test; mkdir ' + stage + ' && mkdir ' + ram)
        for destination, contents in [(stage + '/probe', payload),
                                      (ram + '/relay', relay_payload),
                                      (ram + '/cache-helper', cache_payload)]:
            remote('(set -C; cat > ' + destination + ') && chmod 700 ' + destination,
                   contents, timeout=60 if destination == stage + '/probe' else 15)
        # Flush/drop only these newly uploaded files so staging page cache does not
        # masquerade as the running controller's working memory. Never drop global caches.
        remote(ram + '/cache-helper ' + stage + '/probe')
        fixture_flags = ' '.join(flag for enabled, flag in (
            (args.ble, '--ble'), (args.service_owner, '--service-owner'),
            (args.service_discovery, '--service-discovery'), (args.sensor_reports, '--sensor-reports'),
            (args.icd, '--icd')) if enabled)
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
{ram}/relay {ram}/radio.sock {radio.address} {radio.port} {limit + 5} &
relay=$!
tries=0
while [ ! -S {ram}/radio.sock ]; do
 tries=$((tries + 1)); [ "$tries" -lt 6 ] || exit 1
 kill -0 "$relay" || exit 1
 sleep 1
done
(ulimit -v 8192; MICROHAD_MATTER_TEST_STATE_DIR={stage}/controller-state exec {stage}/probe 'spinel+hdlc+unix://{ram}/radio.sock?no-reset' {fixture_flags}) &
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
    clk_match = re.search(r'RELAY_META clk_tck=(\d+)', output)
    clk_tck = int(clk_match.group(1)) if clk_match else 0
    samples = []
    for line in output.splitlines():
        if not line.startswith('SAMPLE '):
            continue
        values = dict(re.findall(r'([a-z_]+)=([0-9.]+)', line))
        if {'rss_kib', 'memfree_kib', 'free_plus_clean_inactive_file_kib', 'cpu_ticks', 'uptime_s'} <= values.keys():
            samples.append({key: float(value) for key, value in values.items()})
    cpu_seconds = cpu_percent = None
    if clk_tck and len(samples) >= 2:
        wall_seconds = samples[-1]['uptime_s'] - samples[0]['uptime_s']
        cpu_seconds = max(0.0, samples[-1]['cpu_ticks'] - samples[0]['cpu_ticks']) / clk_tck
        if wall_seconds > 0:
            cpu_percent = round(100.0 * cpu_seconds / wall_seconds, 1)
    elapsed_match = re.search(r'PASS commissioning complete elapsed_ms=(\d+)', output)
    commissioning_elapsed_ms = int(elapsed_match.group(1)) if elapsed_match else None
    required_markers = []
    if args.service_owner and not args.icd:
        required_markers.append('PASS production device scheduler catalog restore and radio detach refresh over CASE')
    if args.service_discovery:
        required_markers.extend(('PASS production owned commissioning PASE, provisioning and CASE completion',
                                 'PASS synthetic accessory SRP registration',
                                 'PASS DNS cancelled callback isolation'))
    if args.sensor_reports:
        required_markers.append('PASS production manager sensor reports reach IPC from subscribed CASE reports')
    if args.icd:
        required_markers.extend(('PASS production device manager online snapshot with ICD Management cluster',
                                 'PASS production ICD registration and authenticated Check-In counter acceptance',
                                 'PASS queued CASE read completed during the Check-In active window'))
    report = {'target': args.host, 'radio': 'synthetic RCP', 'real_radio_access': False,
              'linux_ipv6_or_tun': False,
              'encrypted_commissioning_tested': args.commissioning,
              'synthetic_phone_gatt_tested': args.ble,
              'production_owned_commissioner_tested': args.service_owner,
              'production_dns_resolver_tested': args.service_discovery,
              'production_sensor_reports_tested': args.sensor_reports,
              'production_icd_checkin_and_wake_dispatch_tested': args.icd,
              'real_ble_access': False, 'binary_bytes': len(payload),
              'binary_sha256': hashlib.sha256(payload).hexdigest(),
              'memory_preflight_kib': free_ram,
              'observed_peak_rss_kib': max((sample['rss_kib'] for sample in samples), default=None),
              'observed_min_memfree_kib': min((sample['memfree_kib'] for sample in samples), default=None),
              'observed_min_free_plus_clean_inactive_file_kib': min(
                  (sample['free_plus_clean_inactive_file_kib'] for sample in samples), default=None),
              'observed_process_cpu_seconds': round(cpu_seconds, 2) if cpu_seconds is not None else None,
              'observed_average_cpu_percent_one_core': cpu_percent,
              'commissioning_elapsed_ms': commissioning_elapsed_ms,
              'limits': {'rss_kib': rss_limit, 'memfree_floor_kib': 512, 'free_plus_clean_inactive_file_floor_kib': 4096, 'seconds': limit}, 'production_before': before,
              'production_after': after, 'exit_code': exit_code, 'controller_state_reload_tested': 'PASS committed flash fabric reopen' in output,
              'fixture_controller_state_location': 'isolated flash stage',
              'passed': exit_code == 0 and marker in output and not radio_error and
                        all(required in output for required in required_markers),
              'synthetic_radio_error': radio_error, 'upload_cache_dropped_for_test_artifacts': True, 'cleanup_completed': True, 'output': output.splitlines()}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise RuntimeError('Device fixture failed; see ' + str(args.report))
    if before != after:
        raise RuntimeError('Production status changed during test')


if __name__ == '__main__':
    main()
