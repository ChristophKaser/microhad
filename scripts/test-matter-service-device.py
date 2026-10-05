#!/usr/bin/env python3
"""Run a short guarded startup profile of thread-matter on the gateway.

The service uses unique volatile state, IPC and radio paths, waits for the
coordinator observation, and never opens the production UART or service socket.
The production gateway services remain running throughout the check.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / 'build/rs-matter/thread-matter-mips'


def memory_values(meminfo):
    fields = {match.group(1): int(match.group(2)) for match in
              re.finditer(r'^([^:]+):\s+(\d+)', meminfo, re.MULTILINE)}
    free = fields['MemFree']
    inactive_file = fields.get('Inactive(file)', 0)
    unclean = fields.get('Dirty', 0) + fields.get('Writeback', 0) + fields.get('Shmem', 0)
    clean_inactive_file = max(0, inactive_file - unclean)
    return free, clean_inactive_file, free + clean_inactive_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--password-file', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--report', type=Path, default=ROOT / 'logs/matter-service-device-profile.json')
    args = parser.parse_args()
    if not 5 <= args.seconds <= 30:
        parser.error('--seconds must be between 5 and 30')
    if not SERVICE.is_file():
        raise RuntimeError('Missing MIPS service binary: ' + str(SERVICE))
    payload = SERVICE.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    token = uuid.uuid4().hex
    stage = '/tuya/native-test/matter-startup-' + token
    runtime = '/tmp/matter-startup-' + token
    ssh = ['sshpass', '-f', str(args.password_file), 'ssh', '-o', 'ConnectTimeout=8',
           '-o', 'StrictHostKeyChecking=yes', '-o', 'HostKeyAlgorithms=+ssh-rsa',
           '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa', '-o', 'PreferredAuthentications=password',
           '-o', 'PubkeyAuthentication=no', 'root@' + args.host]

    def remote(command, data=None, timeout=20, check=True):
        result = subprocess.run(ssh + [command], input=data, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout, check=False)
        output = result.stdout.decode(errors='replace')
        if check and result.returncode != 0:
            raise RuntimeError(f'Remote command failed ({result.returncode}): {output[-2000:]}')
        return (result.returncode, output) if not check else output

    def status():
        with urllib.request.urlopen('http://' + args.host + '/api/status', timeout=8) as response:
            value = json.load(response)
        return {key: value.get(key) for key in ('online', 'network_formed', 'coordinator_backend')}

    def uptime():
        value = remote("awk '{print $1}' /proc/uptime").strip().splitlines()[-1]
        return float(value)

    before = status()
    uptime_before = uptime()
    if before.get('online') is not True or before.get('network_formed') is not True:
        raise RuntimeError('Production Zigbee service must be online and formed')
    meminfo = remote('cat /proc/meminfo')
    free, clean_cache, effective = memory_values(meminfo)
    if free < 512 or effective < 1536:
        raise RuntimeError(f'Unsafe startup preflight: MemFree={free} KiB, '
                           f'clean inactive cache={clean_cache} KiB, effective={effective} KiB')
    storage = remote('df -k /tuya').splitlines()[-1].split()
    if int(storage[3]) * 1024 < len(payload) + 512 * 1024:
        raise RuntimeError('Insufficient isolated staging space')

    report = {'target': args.host, 'binary_bytes': len(payload), 'binary_sha256': digest,
              'profile_seconds': args.seconds, 'radio_access': False,
              'production_before': before,
              'uptime_before_seconds': uptime_before,
              'guardrails': {'preflight_memfree_floor_kib': 512,
                             'preflight_effective_free_kib': 1536,
                             'runtime_memfree_floor_kib': 512,
                             'deployment_acceptance_memfree_kib': 512,
                             'runtime_effective_free_floor_kib': 1536},
              'memory_preflight': {'memfree_kib': free,
                                   'clean_inactive_file_kib': clean_cache,
                                   'effective_free_kib': effective}}
    guard_aborted = False
    profile_error = None
    started_at = time.time()
    try:
        remote(f'umask 077; mkdir -p /tuya/native-test; mkdir {stage}; mkdir {runtime}')
        remote(f'set -C; cat > {stage}/thread-matter && chmod 700 {stage}/thread-matter', payload)
        # The private URL cannot reach /dev/ttyS1. Without a coordinator
        # observation, production lifecycle deliberately waits before OT init.
        command = f'''umask 077
ulimit -c 0
cd {runtime} || exit 1
{stage}/thread-matter --socket {runtime}/ipc.sock --state-dir {runtime}/state \\
 --radio-url 'spinel+hdlc+unix://{runtime}/radio.sock?no-reset' > {runtime}/service.log 2>&1 &
child=$!
cleanup() {{ kill "$child" 2>/dev/null; wait "$child" 2>/dev/null; }}
trap cleanup EXIT HUP INT TERM
tries=0
while [ ! -S {runtime}/ipc.sock ]; do
 tries=$((tries + 1)); [ "$tries" -le 10 ] || {{ echo IPC_SOCKET_TIMEOUT; cat {runtime}/service.log; exit 1; }}
 kill -0 "$child" 2>/dev/null || {{ echo SERVICE_EXITED_BEFORE_IPC; cat {runtime}/service.log; exit 1; }}
 sleep 1
done
grep 'Matter controller service started' {runtime}/service.log >/dev/null || {{ echo SERVICE_START_MARKER_MISSING; cat {runtime}/service.log; exit 1; }}
count=0
while [ "$count" -lt {args.seconds} ]; do
 count=$((count + 1))
 rss=$(awk '/^VmRSS:/ {{print $2}}' /proc/$child/status)
 ticks=$(awk '{{print $14 + $15}}' /proc/$child/stat)
 uptime=$(awk '{{print $1}}' /proc/uptime)
 mem=$(awk '/^MemFree:/ {{free=$2}} /^Inactive[(]file[)]:/ {{inactive=$2}}
 /^Dirty:/ {{dirty=$2}} /^Writeback:/ {{writeback=$2}} /^Shmem:/ {{shmem=$2}}
 END {{clean=inactive-dirty-writeback-shmem; if(clean<0)clean=0; print free, clean, free+clean}}' /proc/meminfo)
 set -- $mem
 free=$1; cache=$2; effective=$3
 echo "SAMPLE uptime_s=$uptime cpu_ticks=$ticks rss_kib=$rss free_kib=$free inactive_clean_file_kib=$cache effective_free_kib=$effective"
 if [ "${{rss:-0}}" -gt 4096 ] || [ "${{free:-0}}" -lt 512 ] || [ "${{effective:-0}}" -lt 1536 ]; then
  echo TEST_LIMIT_REACHED
  exit 2
 fi
 kill -0 "$child" 2>/dev/null || {{ echo SERVICE_EXITED_DURING_SAMPLE; cat {runtime}/service.log; exit 1; }}
 sleep 1
done
kill -TERM "$child" 2>/dev/null
wait "$child"; result=$?
child=0
cat {runtime}/service.log
echo SERVICE_EXIT=$result
exit "$result"
'''
        exit_code, output = remote('sh -c ' + shlex.quote(command), timeout=args.seconds + 25,
                                   check=False)
        samples = []
        for line in output.splitlines():
            if line.startswith('SAMPLE '):
                samples.append({key: int(value) if key.endswith('_kib') or key == 'cpu_ticks'
                                else float(value) for key, value in
                                re.findall(r'(\w+)=([0-9.]+)', line)})
        report['samples'] = samples
        report['service_log'] = [line for line in output.splitlines()
                                  if line.startswith('Matter controller service ')]
        if exit_code != 0:
            report['profile_failure_markers'] = [line for line in output.splitlines()
                                                 if line.isupper() and ' ' not in line]
        if exit_code != 0 and 'TEST_LIMIT_REACHED' in output:
            guard_aborted = True
            report['result'] = 'ABORTED: runtime memory guard reached'
            report['guard_abort_output'] = 'TEST_LIMIT_REACHED'
        elif (exit_code == 255 and len(samples) >= args.seconds - 2 and
              'Matter controller service started' in output and
              'Matter controller service stopped' in output):
            # This old Dropbear build can close the SSH channel after the
            # child handles SIGTERM even though the complete sample and clean
            # service-stop marker have already arrived.
            report['remote_channel_closed_after_service_stop'] = True
        elif exit_code != 0:
            raise RuntimeError(f'Remote service profile failed ({exit_code}): {output[-2000:]}')
        elif len(samples) < args.seconds - 2:
            raise RuntimeError(f'Insufficient service samples: {len(samples)}')
        elif 'Matter controller service started' not in output or 'Matter controller service stopped' not in output:
            raise RuntimeError('Service did not start and stop cleanly: ' + output[-1200:])
        if not samples:
            raise RuntimeError('No resource samples captured')
        report['rss_max_kib'] = max(row['rss_kib'] for row in samples)
        report['memfree_min_kib'] = min(row['free_kib'] for row in samples)
        report['effective_free_min_kib'] = min(row['effective_free_kib'] for row in samples)
        report['cpu_ticks_delta'] = samples[-1]['cpu_ticks'] - samples[0]['cpu_ticks']
        if not guard_aborted:
            report['result'] = 'PASS startup awaiting coordinator; orderly SIGTERM shutdown'
    except Exception as error:
        profile_error = f'{type(error).__name__}: {error}'
        report['error'] = profile_error
    finally:
        try:
            remote(f'rm -rf {runtime} {stage}; test ! -e {runtime} && test ! -e {stage}')
            report['cleanup_verified'] = True
        except Exception as error:
            report['cleanup_verified'] = False
            report['cleanup_error'] = f'{type(error).__name__}: {error}'
        try:
            report['production_after'] = status()
        except Exception as error:
            report['production_after_error'] = f'{type(error).__name__}: {error}'
        try:
            uptime_after = uptime()
            report['uptime_after_seconds'] = uptime_after
            report['uptime_elapsed_seconds'] = round(uptime_after - uptime_before, 2)
            report['host_elapsed_seconds'] = round(time.time() - started_at, 2)
            report['gateway_reboot_detected'] = uptime_after < uptime_before
            if uptime_after < uptime_before:
                report['error'] = 'Gateway uptime reset during isolated service test'
        except Exception as error:
            report['uptime_after_error'] = f'{type(error).__name__}: {error}'
            report['gateway_reboot_detected'] = None
        if 'production_after' in report and report['production_after'] != before:
            report['production_state_changed'] = True
            report['error'] = 'Production Zigbee state changed during isolated service test'
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if guard_aborted or profile_error or report.get('error') or not report.get('cleanup_verified'):
        return 2
    return 0


if __name__ == '__main__':
    main()
