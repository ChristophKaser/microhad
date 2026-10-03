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
import tempfile
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sim", ROOT / "scripts/simulate-rcp.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


class ThreadRcp(sim.Rcp):
    """Minimal SDK-matched RCP properties; unsupported requests fail explicitly."""
    def reset_properties(self):
        super().reset_properties()
        for props in self.interfaces.values():
            props.update({
                1: sim.packed(4) + sim.packed(3), 8: sim.EUI,
                2: b"synthetic-openthread-rcp\0",
                5: b"".join(sim.packed(cap) for cap in (34, 513)),
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
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=8) as response:
        data = json.load(response)
    # Never log configuration, addresses, keys or the complete status body.
    return {key: data[key] for key in ("online", "network_formed", "coordinator_backend", "state") if key in data}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host")
    parser.add_argument("--password-file", type=Path, default=ROOT.parent / "ssh_password.txt")
    args = parser.parse_args()
    device = args.host or "127.0.0.1"
    token = str(time.time_ns())
    ram = f"/tmp/ot-sim-{token}"
    staged = f"/tuya/native-test/ot-sim-{token}"
    report = {"target": device, "radio": "host simulation only", "checks": [], "commands": []}
    radio = ThreadRcp(device, tcp_nodelay=True)
    process = relay = None
    ssh = sim.ssh_args(args) if args.host else []
    binary_dir = ROOT / "build/openthread-unix"
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    log_path = logs / f"openthread-device-{token}.log"
    lines = []
    events = queue.Queue()

    def remote(command, **kwargs):
        return subprocess.run(ssh + [f"root@{args.host}", command], check=True, timeout=20, **kwargs)

    def upload(source, destination):
        with source.open('rb') as stream:
            remote(f"umask 077; test ! -e {destination} && cat > {destination} && chmod 700 {destination}", stdin=stream)

    def check(label):
        report["checks"].append(label)
        print("PASS", label, flush=True)

    def command(value, timeout=7):
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
        report["commands"].append({"command": value, "response": result})
        return "\n".join(result)

    try:
        if args.host:
            report["production_before"] = status(args.host)
            memory = remote("cat /proc/meminfo", stdout=subprocess.PIPE, text=True).stdout
            free = int(re.search(r"MemFree:\s+(\d+)", memory).group(1))
            report["memory_preflight_kib"] = free
            # No large RAM-backed upload: only executable mappings and small state.
            if free < 3072:
                raise RuntimeError(f"Need 3072 KiB genuinely free for bounded simulation; found {free}")
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
{staged}/relay {ram}/radio.sock {radio.address} {radio.port} &
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
  free=$(awk '/^MemFree:/ {{print $2}}' /proc/meminfo)
  echo "SAMPLE rss_kib=$rss free_kib=$free"
  if [ "$count" -ge 35 ] || [ "${{rss:-0}}" -gt 2048 ] || [ "${{free:-0}}" -lt 1024 ]; then
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
            process = subprocess.Popen(ssh + [f"root@{args.host}", "sh -c " + shlex.quote(script)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        else:
            os.mkdir(ram, 0o700)
            relay = subprocess.Popen([str(binary_dir / 'sim-relay-host'), ram+'/radio.sock', radio.address, str(radio.port)])
            deadline = time.monotonic() + 3
            while not Path(ram+'/radio.sock').exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Host relay startup failed")
                time.sleep(0.02)
            process = subprocess.Popen([str(binary_dir / 'host/src/posix/ot-cli'), '-v',
                f'spinel+hdlc+unix://{ram}/radio.sock?no-reset'], cwd=ram,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        def collect():
            with log_path.open('w') as log:
                for line in process.stdout:
                    line = line.rstrip()
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
        report['samples'] = [line for line in lines if line.startswith('SAMPLE ')]
        radio.close()
        report_path = logs / f'openthread-device-{token}.json'
        report_path.write_text(json.dumps(report, indent=2)+'\n')
        print('Report:', report_path, flush=True)
        print('Log:', log_path, flush=True)
    return 1 if 'error' in report else 0


if __name__ == '__main__':
    raise SystemExit(main())
