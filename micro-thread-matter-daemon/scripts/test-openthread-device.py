#!/usr/bin/env python3
"""Run the real OpenThread CLI against an explicitly synthetic RCP.

With --host, stage only new test binaries below /tuya/native-test, use volatile
private settings, bound execution/memory, and clean up. Never opens the UART.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import queue
import re
import shlex
import socket
import struct
import subprocess
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT / "scripts"))
from toolchain_config import target_path, settings

spec = importlib.util.spec_from_file_location("sim", ROOT / "ziggurat/scripts/simulate-rcp.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)
from workspace_config import setting, gateway_host
from gateway_api import urlopen


class ThreadRcp(sim.Rcp):
    """Minimal firmware-compatible RCP properties; unsupported requests fail explicitly."""
    def reset_properties(self):
        super().reset_properties()
        for props in self.interfaces.values():
            props.update({
                1: sim.packed(4) + sim.packed(3), 8: sim.EUI,
                2: b"synthetic-openthread-rcp\0",
                5: b"".join(sim.packed(cap) for cap in (34, 513, 64, 65)),
                0xb0: sim.packed(11), 0xb1: sim.packed(4),
                0x120b: sim.packed(109),
                32: b"\0", 33: b"\x0b", 34: bytes(range(11, 27)),
                36: struct.pack("b", -75), 37: b"\0", 38: struct.pack("b", -40),
                41: bytes(range(11, 27)), 39: struct.pack("b", -100),
                54: sim.EUI, 52: b"\xff\xff", 53: b"\xfe\xff", 55: b"\0", 56: b"\0", 59: b"\x01",
                0x1302: sim.EUI[::-1], 0x1303: b"\0", 0x1304: b"", 0x1305: b"",
                0x800: b"", 0x801: b"\0" * 4,
            })
        self.requests = []

    def receive_frame(self, data):
        command, offset = sim.unpacked(data, 1)
        iid = data[0] >> 4 & 3
        if command == 1:
            self.counts['command_1'] += 1
            raise RuntimeError("Unexpected reset in no-reset simulation")
        prop, offset = sim.unpacked(data, offset)
        self.requests.append({"command": command, "iid": iid, "property": prop})
        self.counts[f"command_{command}"] += 1
        self.counts[f"iid_{iid}_property_{prop}"] += 1
        props = self.interfaces.get(iid)
        if props is None:
            self.send(data[0], 0, sim.packed(6))
        elif command == 2 and prop == 0x802:
            self.send(data[0], prop, struct.pack("<Q", time.monotonic_ns() // 1000))
        elif command == 2 and prop in props:
            self.send(data[0], prop, props[prop])
        elif command == 3 and prop == 113:
            # Explicitly synthetic TX completion: no peer exists to acknowledge unicast.
            value = data[offset:]
            length = struct.unpack_from('<H', value)[0]
            if not 0 < length <= 127 or len(value) < length + 3:
                raise ValueError('Invalid simulated radio frame')
            self.counts['radio_tx_frames'] += 1
            if value[2] & 0x20:
                self.counts['radio_tx_no_ack'] += 1
                self.send(data[0], 0, sim.packed(17) + b'\0\0')
            else:
                # Empty ACK with the SDK's required radio metadata and MAC-data struct.
                ack = struct.pack('<HbbH', 0, 0, -128, 0)
                ack += struct.pack('<HBBQ', 10, value[2 + length], 0, time.monotonic_ns() // 1000)
                ack += struct.pack('<HB', 1, 0) + struct.pack('<HBI', 5, 0, 0)
                self.send(data[0], 0, sim.packed(0) + b'\0\0' + ack)
        elif command == 3 and prop in props:
            props[prop] = data[offset:]
            self.send(data[0], prop, props[prop])
        elif command in (4, 5) and prop in (0x1304, 0x1305):
            # Source-match list operations are accepted only in the synthetic RCP.
            self.conn.sendall(sim.encode(bytes([data[0], command + 3]) + sim.packed(prop) + data[offset:]))
        else:
            self.counts[f"unsupported_{command}_{prop}"] += 1
            self.send(data[0], 0, sim.packed(2))


def status(host):
    with urlopen(f"http://{host}/api/status", timeout=8) as response:
        data = json.load(response)
    # Never log configuration, addresses, keys or the complete status body.
    return {key: data[key] for key in ("online", "network_formed", "coordinator_backend", "state") if key in data}


def memory_values(meminfo):
    """Return MemFree and conservatively reclaimable inactive file cache."""
    fields = {match.group(1): int(match.group(2)) for match in
              re.finditer(r"^([^:]+):\s+(\d+)", meminfo, re.MULTILINE)}
    free = fields["MemFree"]
    inactive_file = fields.get("Inactive(file)", 0)
    unclean = fields.get("Dirty", 0) + fields.get("Writeback", 0) + fields.get("Shmem", 0)
    clean_inactive_file = max(0, inactive_file - unclean)
    return free, clean_inactive_file, free + clean_inactive_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', nargs='?', const=setting('GATEWAY_IP'), type=gateway_host)
    parser.add_argument("--password-file", type=Path, default=None)
    parser.add_argument('--profile', action='store_true', help='20s disabled idle, 60s loaded UDP/RCP calls, 20s enabled idle')
    parser.add_argument('--profile-load-seconds', type=int, default=60)
    parser.add_argument('--constrained-memory-profile', action='store_true',
                        help='use 2560 KiB preflight, 1024 KiB process RSS and 1536 KiB MemFree limits')
    args = parser.parse_args()
    if not 5 <= args.profile_load_seconds <= 120:
        parser.error('--profile-load-seconds must be between 5 and 120')
    if args.constrained_memory_profile and (not args.profile or not args.host):
        parser.error('--constrained-memory-profile requires --profile and --host')
    preflight_free_kib = 2560 if args.constrained_memory_profile else 3072
    preflight_memfree_floor_kib = 1536
    process_rss_limit_kib = 1024 if args.constrained_memory_profile else 2048
    runtime_free_limit_kib = 1536 if args.constrained_memory_profile else 1024
    runtime_memfree_floor_kib = 1024
    limit_seconds = (args.profile_load_seconds + 75) if args.profile else 35
    relay_seconds = limit_seconds + 10
    phase = 'startup'
    resource_samples = []
    device = args.host or "127.0.0.1"
    token = str(time.time_ns())
    ram = f"/tmp/ot-sim-{token}"
    staged = f"/tuya/native-test/ot-sim-{token}"
    report = {"target": device, "radio": "host simulation only", "checks": [], "commands": [],
              "guardrails": {"preflight_effective_free_kib": preflight_free_kib,
                             "preflight_memfree_floor_kib": preflight_memfree_floor_kib,
                             "process_rss_limit_kib": process_rss_limit_kib,
                             "runtime_effective_free_limit_kib": runtime_free_limit_kib,
                             "runtime_memfree_floor_kib": runtime_memfree_floor_kib,
                             "virtual_memory_limit_kib": 4096}}
    radio = ThreadRcp(device, tcp_nodelay=True)
    process = relay = None
    ssh = sim.ssh_args(args) if args.host else []
    binary_dir = target_path(ROOT / "build/openthread-unix") if args.host else ROOT / "build/openthread-unix"
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    log_path = logs / f"openthread-device-{token}.log"
    lines = []
    events = queue.Queue()

    def remote(command, **kwargs):
        return subprocess.run(ssh + [args.host, command], check=True, timeout=20, **kwargs)

    def upload(source, destination):
        with source.open('rb') as stream:
            remote(f"umask 077; test ! -e {destination} && cat > {destination} && chmod 700 {destination}", stdin=stream)

    def check(label):
        report["checks"].append(label)
        print("PASS", label, flush=True)

    def command(value, timeout=7, record=True):
        process.stdin.write(value + "\n")
        process.stdin.flush()
        result = []
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"CLI command timed out: {value}")
            try:
                line = events.get(timeout=remaining)
            except queue.Empty:
                raise TimeoutError(f"CLI command timed out: {value}") from None
            if line is None:
                raise RuntimeError(f"CLI ended during {value}: " + "\n".join(lines[-12:]))
            result.append(line)
            if "Error " in line:
                raise RuntimeError(f"CLI rejected {value}: {line}")
            if line.strip().removeprefix('> ').strip() == 'Done':
                break
        if record:
            report["commands"].append({"command": value, "response": result})
        return "\n".join(result)

    try:
        if args.host:
            report["production_before"] = status(args.host)
            memory = remote("cat /proc/meminfo", stdout=subprocess.PIPE, text=True).stdout
            free, clean_inactive_file, effective_free = memory_values(memory)
            report["memory_preflight_memfree_kib"] = free
            report["memory_preflight_clean_inactive_file_kib"] = clean_inactive_file
            report["memory_preflight_effective_free_kib"] = effective_free
            # No large RAM-backed upload: only executable mappings and small state.
            if effective_free < preflight_free_kib or free < preflight_memfree_floor_kib:
                raise RuntimeError("Need at least " + str(preflight_free_kib) +
                                   " KiB effective free and " + str(preflight_memfree_floor_kib) +
                                   " KiB MemFree; found effective=" + str(effective_free) +
                                   " KiB, MemFree=" + str(free) + " KiB")
            storage = remote("df -k /tuya", stdout=subprocess.PIPE, text=True).stdout
            available = int(storage.splitlines()[-1].split()[3])
            if available < 2048:
                raise RuntimeError("Need 2 MiB free flash for temporary isolated staging")
            remote(f"umask 077; mkdir -p /tuya/native-test; mkdir {staged}; mkdir {ram}")
            upload(binary_dir / 'ot-cli-mips', staged + '/ot-cli')
            upload(binary_dir / 'sim-relay-mips', staged + '/relay')
            # Smoke test before any stack startup; settings are never touched by help.
            help_result = remote(f"ulimit -c 0; {staged}/ot-cli --help", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            assert "Syntax:" in help_result.stdout
            check("MIPS executable runs --help on factory Linux")
            script = f'''set -u
exec 3<&0
ulimit -c 0
cd {ram} || exit 1
{staged}/relay {ram}/radio.sock {radio.address} {radio.port} {relay_seconds} &
relay_pid=$!
child=0
watchdog=0
cleanup() {{
 [ "$child" = 0 ] || kill "$child" 2>/dev/null
 [ "$watchdog" = 0 ] || kill "$watchdog" 2>/dev/null
 kill "$relay_pid" 2>/dev/null
 wait "$relay_pid" 2>/dev/null
 rm -rf {ram} {staged}
}}
trap cleanup EXIT HUP INT TERM
tries=0
while [ ! -S {ram}/radio.sock ]; do
 tries=$((tries + 1)); [ "$tries" -lt 6 ] || exit 1
 kill -0 "$relay_pid" || exit 1
 sleep 1
done
(ulimit -v 4096; exec {staged}/ot-cli -v 'spinel+hdlc+unix://{ram}/radio.sock?no-reset') <&3 &
child=$!
(
 count=0
 while kill -0 "$child" 2>/dev/null; do
  count=$((count + 1))
  rss=$(awk '/^VmRSS:/ {{print $2}}' /proc/$child/status)
  memory=$(awk '/^MemFree:/ {{free=$2}} /^Inactive[(]file[)]:/ {{inactive=$2}}
   /^Dirty:/ {{dirty=$2}} /^Writeback:/ {{writeback=$2}} /^Shmem:/ {{shmem=$2}}
   END {{clean=inactive-dirty-writeback-shmem; if(clean<0)clean=0; print free, clean, free+clean}}' /proc/meminfo)
  set -- $memory
  free=$1
  clean_inactive_file=$2
  effective_free=$3
  ticks=$(awk '{{print $14 + $15}}' /proc/$child/stat)
  uptime=$(awk '{{print $1}}' /proc/uptime)
  threads=$(awk '/^Threads:/ {{print $2}}' /proc/$child/status)
  relay_rss=$(awk '/^VmRSS:/ {{print $2}}' /proc/$relay_pid/status)
  relay_ticks=$(awk '{{print $14 + $15}}' /proc/$relay_pid/stat)
  echo "SAMPLE uptime_s=$uptime cpu_ticks=$ticks rss_kib=$rss free_kib=$free inactive_clean_file_kib=$clean_inactive_file effective_free_kib=$effective_free threads=$threads relay_rss_kib=$relay_rss relay_cpu_ticks=$relay_ticks"
  if [ "$count" -ge {limit_seconds} ] || [ "${{rss:-0}}" -gt {process_rss_limit_kib} ] || [ "${{free:-0}}" -lt {runtime_memfree_floor_kib} ] || [ "${{effective_free:-0}}" -lt {runtime_free_limit_kib} ]; then
   echo TEST_LIMIT_REACHED
   kill -9 "$child" 2>/dev/null
   exit 1
  fi
  sleep 1
 done
) &
watchdog=$!
wait "$child"
result=$?
echo "CLI_EXIT=$result"
exit "$result"
'''
            process = subprocess.Popen(ssh + [args.host, "sh -c " + shlex.quote(script)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        else:
            os.mkdir(ram, 0o700)
            relay = subprocess.Popen([str(binary_dir / 'sim-relay-host'), ram+'/radio.sock', radio.address, str(radio.port), str(relay_seconds)])
            deadline = time.monotonic() + 3
            while not Path(ram+'/radio.sock').exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Host relay startup failed")
                time.sleep(0.02)
            process = subprocess.Popen([str(binary_dir / 'host/src/posix/ot-cli'), '-v',
                f'spinel+hdlc+unix://{ram}/radio.sock?no-reset'], cwd=ram,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        def collect():
            pending = ''
            with log_path.open('w') as log:
                for line in process.stdout:
                    line = line.rstrip()
                    # The watchdog and CLI share stdout; a sample may land between
                    # the CLI's response text and its terminating newline.
                    sample = re.search(r'SAMPLE (?:\w+=[0-9.]+ ?){9}', line)
                    if sample:
                        fields = dict(re.findall(r'(\w+)=([0-9.]+)', sample.group()))
                        resource_samples.append({**{key: float(value) for key, value in fields.items()}, 'phase': phase})
                        lines.append(sample.group().strip())
                        log.write(sample.group().strip()+'\n')
                        prefix, suffix = line[:sample.start()], line[sample.end():]
                        if not suffix:
                            pending += prefix
                            log.flush()
                            continue
                        line = prefix + suffix
                    line = pending + line
                    pending = ''
                    lines.append(line)
                    log.write(line+'\n')
                    log.flush()
                    events.put(line)
            events.put(None)
        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        assert 'OPENTHREAD' in command('version').upper()
        check("OpenThread initializes against synthetic RCP through Unix adapter")
        assert '0200000000000042' in command('eui64')
        assert 'disabled' in command('state')
        assert '11' in command('channel')
        command('channel 15')
        assert '15' in command('channel')
        check("CLI identity, state and channel read/write work")
        if args.profile:
            phase = 'disabled_idle'
            check('Profiling 20 seconds of disabled idle')
            time.sleep(20)
        command('dataset init new')
        command('dataset networkname OpenThreadSim')
        command('dataset commit active')
        command('ifconfig up')
        command('thread start')
        address_output = command('ipaddr mleid')
        addresses = re.findall(r'(?m)^(?:> )?([0-9a-f:]+)$', address_output)
        assert addresses, address_output
        address = addresses[0]
        command('udp open')
        command('udp bind :: 12345')
        received = command(f'udp send {address} 12345 simulator-loopback')
        deadline = time.monotonic() + 3
        while 'simulator-loopback' not in received and time.monotonic() < deadline:
            try:
                line = events.get(timeout=3)
            except queue.Empty:
                break
            if line is None:
                break
            received += '\n'+line
        assert re.search(r'18 bytes from .* 12345 simulator-loopback', received), received
        check("Internal IPv6/UDP sends to own Thread address without Linux IPv6/TUN")
        if args.profile:
            phase = 'loaded'
            load_started = time.monotonic()
            cycles = 0
            payload = 'x' * 256
            latencies = []
            while time.monotonic() - load_started < args.profile_load_seconds:
                cycle_started = time.monotonic()
                command('ccathreshold', record=False)
                command('txpower', record=False)
                output = command(f'udp send {address} 12345 {payload}', record=False)
                receive_deadline = time.monotonic() + 3
                while not re.search(r'256 bytes from .* 12345 ' + payload, output):
                    remaining = receive_deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('Profile UDP receive timed out')
                    line = events.get(timeout=remaining)
                    if line is None:
                        raise RuntimeError('CLI ended during profile UDP receive')
                    output += '\n' + line
                cycles += 1
                latencies.append((time.monotonic() - cycle_started) * 1000)
                pause = 0.05 - (time.monotonic() - cycle_started)
                if pause > 0:
                    time.sleep(pause)
            elapsed = time.monotonic() - load_started
            report['profile_workload'] = {
                'loaded_seconds': elapsed, 'completed_cycles': cycles,
                'cycles_per_second': cycles / elapsed,
                'udp_payload_bytes': 256, 'udp_payload_bytes_total': cycles * 256,
                'radio_get_calls': cycles * 2,
                'cycle_latency_ms': sim.timing_summary(latencies),
                'description': 'up to 20 cycles/s: ccathreshold GET, txpower GET, 256-byte internal UDP send/receive',
            }
            check(f'Completed {cycles} UDP/radio-query cycles over {elapsed:.1f} seconds')
            phase = 'enabled_idle'
            check('Profiling 20 seconds of enabled idle')
            time.sleep(20)
        phase = 'shutdown'
        command('udp close')
        command('thread stop')
        command('ifconfig down')
        process.stdin.close()
        result = process.wait(timeout=12)
        reader.join(2)
        report['exit_code'] = result
        assert result == 0, result
        assert not radio.error, radio.error
        assert not radio.counts.get('resets', 0)
        check("Clean exit with no radio reset commands")
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        print('FAIL', error, flush=True)
    finally:
        if process and process.poll() is None:
            if process.stdin and not process.stdin.closed:
                process.stdin.close()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        if relay:
            relay.terminate()
            relay.wait(timeout=3)
        if args.host:
            # Exact unique paths only; never touch persistent service files/state.
            remote(f"rm -rf {ram} {staged}; test ! -e {ram} && test ! -e {staged}")
            report['cleanup_verified'] = True
            report['production_after'] = status(args.host)
            for field in ('online', 'network_formed', 'coordinator_backend'):
                if field in report.get('production_before', {}):
                    assert report['production_after'].get(field) == report['production_before'][field], field
            check('Temporary binaries/settings/socket removed; production dashboard responds')
        else:
            import shutil
            shutil.rmtree(ram, ignore_errors=True)
        report['requests'] = radio.requests
        report['simulator_counts'] = dict(radio.counts)
        report['simulator_error'] = radio.error
        report['samples'] = [line.removeprefix('> ') for line in lines if line.removeprefix('> ').startswith('SAMPLE ')]
        report['resource_samples'] = resource_samples
        metadata = next((line for line in lines if line.startswith('RELAY_META ')), '')
        clock_match = re.search(r'clk_tck=(\d+)', metadata)
        if clock_match and resource_samples:
            hz = int(clock_match.group(1))
            report['clock_ticks_per_second'] = hz
            summaries = {}
            for label in ('startup', 'disabled_idle', 'loaded', 'enabled_idle', 'shutdown'):
                points = [point for point in resource_samples if point['phase'] == label]
                if not points:
                    continue
                intervals = [(b['uptime_s'] - a['uptime_s'], b['cpu_ticks'] - a['cpu_ticks'],
                              b['relay_cpu_ticks'] - a['relay_cpu_ticks']) for a, b in zip(points, points[1:])
                             if b['uptime_s'] > a['uptime_s']]
                elapsed = sum(item[0] for item in intervals)
                summaries[label] = {
                    'sample_count': len(points),
                    'sampled_rss_min_kib': min(point['rss_kib'] for point in points),
                    'sampled_rss_max_kib': max(point['rss_kib'] for point in points),
                    'sampled_free_min_kib': min(point['free_kib'] for point in points),
                    'sampled_clean_inactive_file_min_kib': min(point['inactive_clean_file_kib'] for point in points),
                    'sampled_effective_free_min_kib': min(point['effective_free_kib'] for point in points),
                    'max_threads': max(point['threads'] for point in points),
                    'measured_interval_seconds': elapsed,
                    'cpu_mean_percent_one_core': sum(item[1] for item in intervals) / hz / elapsed * 100 if elapsed else None,
                    'cpu_peak_interval_percent_one_core': max(item[1] / hz / item[0] * 100 for item in intervals) if intervals else None,
                    'relay_cpu_mean_percent_one_core': sum(item[2] for item in intervals) / hz / elapsed * 100 if elapsed else None,
                    'relay_sampled_rss_max_kib': max(point['relay_rss_kib'] for point in points),
                }
            report['resource_summary'] = summaries
            report['cpu_scope'] = 'OpenThread main PID utime+stime / remote uptime; Threads checked; relay separately measured; SSH/sampler/production excluded'
        if args.profile and not report.get('error') and args.host:
            if len(resource_samples) < 10 or not report.get('resource_summary', {}).get('loaded', {}).get('measured_interval_seconds'):
                report['error'] = 'Insufficient resource samples for profile'
            if any(point['threads'] != 1 for point in resource_samples):
                report['error'] = 'Unexpected worker threads; main-PID CPU scope incomplete'
        radio.close()
        report_path = logs / f'openthread-device-{token}.json'
        report_path.write_text(json.dumps(report, indent=2)+'\n')
        print('Report:', report_path, flush=True)
        print('Log:', log_path, flush=True)
    return 1 if 'error' in report else 0


if __name__ == '__main__':
    raise SystemExit(main())
