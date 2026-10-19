#!/usr/bin/env python3
"""Bounded host Spinel simulator and protocol-v2 client for the stock gateway.

Only generated test state is sent. No production configuration is read.
"""
import argparse
import base64
from collections import Counter, deque
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import statistics
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
EUI = bytes.fromhex("0200000000000042")
SPECIAL = {0x7e, 0x7d, 0x11, 0x13, 0xf8}


import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, gateway_host, ssh_prefix


def packed(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def unpacked(data, offset):
    value = 0
    for shift in (0, 7, 14):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("Invalid Spinel packed integer")


def crc(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x8408 if value & 1 else 0)
    return value ^ 0xffff


def encode(data):
    framed = bytearray([0x7e])
    for byte in data + struct.pack("<H", crc(data)):
        if byte in SPECIAL:
            framed.extend((0x7d, byte ^ 0x20))
        else:
            framed.append(byte)
    return bytes(framed + b"\x7e")


def decode(data):
    result = bytearray()
    escaped = False
    for byte in data:
        if escaped:
            result.append(byte ^ 0x20)
            escaped = False
        elif byte == 0x7d:
            escaped = True
        else:
            result.append(byte)
    if escaped or len(result) < 2 or crc(result[:-2]) != struct.unpack("<H", result[-2:])[0]:
        raise ValueError("Invalid HDLC checksum or escape")
    return bytes(result[:-2])


class Rcp:
    def __init__(self, device, tcp_nodelay=False, iid=0):
        self.tcp_nodelay = tcp_nodelay
        route = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        route.connect((device, 9))  # Route selection only; no datagram is sent.
        self.address = route.getsockname()[0]
        route.close()
        self.listener = socket.socket()
        self.listener.bind((self.address, 0))
        self.listener.listen(1)
        self.listener.settimeout(1)
        self.port = self.listener.getsockname()[1]
        self.device = device
        self.conn = None
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.counts = Counter()
        self.iid = iid
        self.reset_properties()
        self.error = None
        self.response_times_ms = []
        self.beacon_times_ms = []
        self.beacon_pending = deque()
        self.beacon_lock = threading.Lock()
        self.worker = threading.Thread(target=self.run, daemon=True)
        self.worker.start()

    def reset_properties(self):
        ids = (1, 2) if getattr(self, "iid", 0) else (0,)
        self.interfaces = {iid: {8: EUI[::-1], 2: b"host-simulated-rcp\x00"} for iid in ids}
        self.properties = self.interfaces[getattr(self, "iid", 0)]

    def send(self, header, prop, value):
        with self.lock:
            self.conn.sendall(encode(bytes([header, 6]) + packed(prop) + value))

    def receive_frame(self, data):
        command, offset = unpacked(data, 1)
        self.counts[f"command_{command}"] += 1
        iid = (data[0] >> 4) & 3
        interfaces = getattr(self, "interfaces", {0: self.properties})
        if iid not in interfaces:
            self.send(data[0], 0, packed(6))
            return
        if command == 1:
            self.counts["resets"] += 1
            self.reset_properties()
            self.send(0x80, 0, packed(114))
            return
        properties = interfaces[iid]
        prop, offset = unpacked(data, offset)
        self.counts[f"iid_{iid}_property_{prop}"] += 1
        self.counts[f"property_{prop}"] += 1
        if command == 2 and prop in properties:
            self.send(data[0], prop, properties[prop])
        elif command == 3:
            value = data[offset:]
            if prop == 113:
                length = struct.unpack("<H", value[:2])[0]
                if not 0 < length <= 127 or len(value) < length + 2:
                    raise ValueError("Invalid transmitted PSDU length")
                self.counts[f"iid_{iid}_tx_frames"] += 1
                self.counts["tx_frames"] += 1
                self.counts["tx_bytes"] += length
                if value[2] & 7 == 0:
                    self.counts["beacon_replies"] += 1
                    with self.beacon_lock:
                        if self.beacon_pending:
                            self.beacon_times_ms.append((time.perf_counter_ns() - self.beacon_pending.popleft()) / 1e6)
                self.send(data[0], 0, b"\x00")
            elif prop in {32, 33, 37, 52, 53, 54, 55, 56, 59, 4867, 4868, 4869}:
                properties[prop] = value
                self.send(data[0], prop, value)
            else:
                self.send(data[0], 0, packed(2))  # Unimplemented, never silently succeed.
        else:
            self.send(data[0], 0, packed(2))

    def inject(self, psdu, iid=None):
        iid = self.iid if iid is None else iid
        # Raw receive framing includes an FCS, removed by SpinelPhy.
        frame = psdu + b"\x00\x00"
        metadata = struct.pack("<bbIBBQB", -42, -100, 0, 15, 200, 123456, 0)
        if len(psdu) == 8 and psdu[:2] == b"\x03\x08" and psdu[-1] == 7:
            with self.beacon_lock:
                self.beacon_pending.append(time.perf_counter_ns())
        self.send(0x80 | iid << 4, 113, struct.pack("<H", len(frame)) + frame + metadata)
        self.counts["rx_injected"] += 1

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    conn, peer = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            if peer[0] != self.device:
                conn.close()
                raise ValueError(f"Unexpected RCP peer: {peer[0]}")
            self.conn = conn
            if self.tcp_nodelay:
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.settimeout(1)
            buffer = bytearray()
            while not self.stop.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                for byte in chunk:
                    if byte == 0x7e:
                        if buffer:
                            response_started = time.perf_counter_ns()
                            self.receive_frame(decode(buffer))
                            self.response_times_ms.append((time.perf_counter_ns() - response_started) / 1e6)
                            buffer.clear()
                    else:
                        buffer.append(byte)
                        if len(buffer) > 4096:
                            raise ValueError("Oversized HDLC frame")
        except Exception as error:
            if not self.stop.is_set():
                self.error = str(error)

    def close(self):
        self.stop.set()
        if self.conn:
            self.conn.close()
        self.listener.close()
        self.worker.join(2)


class Client:
    tcp_nodelay = False

    def __init__(self, port, timings=None):
        self.conn = socket.create_connection(("127.0.0.1", port), timeout=5)
        if self.tcp_nodelay:
            self.conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode()
        self.conn.sendall((f"GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        header = bytearray()
        while not header.endswith(b"\r\n\r\n"):
            header.extend(self.exact(1))
            if len(header) > 8192:
                raise ValueError("Oversized WebSocket handshake")
        expected = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        if not header.startswith(b"HTTP/1.1 101 ") or expected not in header:
            raise ValueError("WebSocket handshake failed")
        self.sequence = 0
        self.timings = timings if timings is not None else []
        self.events = []
        self.hello = self.receive()
        assert self.hello[:4] == b"\x00\x03\x00\x02", self.hello

    def exact(self, length):
        result = bytearray()
        while len(result) < length:
            chunk = self.conn.recv(length - len(result))
            if not chunk:
                raise ConnectionError("WebSocket closed")
            result.extend(chunk)
        return bytes(result)

    def websocket_frame(self):
        first, second = self.exact(2)
        assert first & 0x80 and not second & 128, (first, second)
        length = second & 127
        if length == 126:
            length = struct.unpack("!H", self.exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.exact(8))[0]
        assert length <= 65536
        return first & 15, self.exact(length)

    def receive(self):
        opcode, data = self.websocket_frame()
        assert opcode == 2, opcode
        return data

    def send(self, opcode, data):
        mask = os.urandom(4)
        length = len(data)
        header = bytes([0x80 | opcode, 128 | length]) if length < 126 else bytes([0x80 | opcode, 0xfe]) + struct.pack("!H", length)
        self.conn.sendall(header + mask + bytes(byte ^ mask[i % 4] for i, byte in enumerate(data)))

    def request(self, command, payload=b"", status=0):
        self.sequence += 1
        data = bytes([command]) + struct.pack("<H", self.sequence << 2) + payload
        started = time.perf_counter_ns()
        self.send(2, data)
        while True:
            frame = self.receive()
            if frame[:3] == bytes([command]) + struct.pack("<H", (self.sequence << 2) | 1):
                assert frame[3] == status, (command, frame.hex())
                self.timings.append({"command": command, "status": status,
                                     "elapsed_ms": (time.perf_counter_ns() - started) / 1e6})
                return frame[4:]
            self.events.append(frame)

    def close(self):
        try:
            self.send(8, struct.pack("!H", 1000))
            for _ in range(128):
                opcode, _ = self.websocket_frame()
                if opcode == 8:
                    break
        except (OSError, AssertionError):
            pass
        finally:
            self.conn.close()


def config():
    # Disposable synthetic coordinator state, never copied from gateway data.
    return (b"\x00\x01" + struct.pack("<BBH", 15, 0, 0x1a62)
            + bytes.fromhex("0200000000000062") + b"\x00\x00" + EUI
            + bytes(range(16)) + b"\x00" + struct.pack("<I", 0)
            + bytes(range(16, 32)) + b"\x00" + bytes(16) + b"\x01\x08"
            + struct.pack("<I", 0))


def ssh_args(args):
    return ssh_prefix(args.password_file)


def timing_summary(values):
    if not values:
        return None
    ordered = sorted(values)
    # Nearest-rank quantiles keep single-operation samples interpretable.
    def percentile(percent):
        import math
        return ordered[max(0, math.ceil(len(ordered) * percent / 100) - 1)]
    return {"count": len(values), "min_ms": ordered[0], "mean_ms": statistics.mean(values),
            "median_ms": statistics.median(values), "p95_ms": percentile(95),
            "p99_ms": percentile(99), "max_ms": ordered[-1]}


def top_samples(output):
    """Sum LinuxThreads descendants, which have their own PIDs on stock uClibc."""
    root_pid = next((int(line.split()[1]) for line in output if line.startswith("TOP_PID ")), None)
    frames = {}
    for line in output:
        if line.startswith("TOP_ROW "):
            _, frame, pid, parent, cpu, vsz = line.split()
            frames.setdefault(int(frame), []).append({"pid": int(pid), "parent": int(parent),
                "cpu_percent": float(cpu.rstrip("%")), "vsz_kib": int(vsz)})
    result = []
    for frame, rows in sorted(frames.items()):
        if frame == 1 or not any(row["pid"] == root_pid for row in rows):
            continue
        owned = {root_pid}
        while True:
            added = {row["pid"] for row in rows if row["parent"] in owned} - owned
            if not added:
                break
            owned.update(added)
        members = [row for row in rows if row["pid"] in owned]
        result.append({"frame": frame, "cpu_percent": sum(row["cpu_percent"] for row in members),
                       "threads": members})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=setting("GATEWAY_IP"), type=gateway_host)
    parser.add_argument("--password-file", type=Path, default=None)
    parser.add_argument("--binary", default="/tmp/ziggurat-server-smoke-20261002")
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--load", action="store_true")
    parser.add_argument("--rates", default="5,20,50,100")
    parser.add_argument("--stage-seconds", type=int, default=6)
    parser.add_argument("--unlimited-broadcasts", action="store_true")
    parser.add_argument("--tcp-nodelay", action="store_true")
    args = parser.parse_args()
    if not args.binary.startswith("/tmp/ziggurat-server-smoke-") or not 1 <= args.seconds <= 25:
        parser.error("Use a temporary smoke binary and a 1–25 second soak")
    rates = [int(rate) for rate in args.rates.split(",")]
    if args.load and (not rates or any(not 1 <= rate <= 100 for rate in rates)
                      or not 4 <= args.stage_seconds <= 8 or len(rates) * (args.stage_seconds + 4) > 44):
        parser.error("Use rates 1–100/s and 4–8 seconds per stage, with at most 44 seconds including drain")
    # Independent upstream HDLC vector, not only an encode/decode round trip.
    assert encode(bytes.fromhex("810202")) == bytes.fromhex("7e8102025e807e")
    Client.tcp_nodelay = args.tcp_nodelay
    rcp = Rcp(args.host, args.tcp_nodelay)
    reservation = socket.socket()
    reservation.bind(("127.0.0.1", 0))
    client_port = reservation.getsockname()[1]
    reservation.close()
    remote_port = 19997
    token = str(time.time_ns())
    remote_log = f"/tmp/ziggurat-simulation-{token}.log"
    top_log = remote_log + ".top"
    remote = f"""set -u
ulimit -c 0 || exit 1
test -x {shlex.quote(args.binary)} || exit 2
test ! -e {remote_log} || exit 2
{shlex.quote(args.binary)} --device tcp://{rcp.address}:{rcp.port} --listen 127.0.0.1:{remote_port} --log-level info > {remote_log} 2>&1 &
child=$!
echo TOP_PID "$child"
top -b -d 1 -n 65 > {top_log} 2>&1 &
top_pid=$!
(sleep 70; kill -9 "$child" 2>/dev/null) &
watchdog=$!
cleanup() {{
    kill "$top_pid" "$child" "$watchdog" 2>/dev/null
    wait "$top_pid" 2>/dev/null
    wait "$child" 2>/dev/null
    wait "$watchdog" 2>/dev/null
    cat {remote_log}
    awk '/^CPU:/ {{frame++; print "TOP_CPU", frame, $0}} $1 ~ /^[0-9]+$/ && NF >= 8 {{print "TOP_ROW", frame, $1, $2, $7, $5}}' {top_log}
}}
trap cleanup EXIT HUP INT TERM
sleep 1
kill -0 "$child" 2>/dev/null || exit 1
echo READY
while read action; do
    case "$action" in
        sample) echo SAMPLE; cat /proc/$child/stat; awk '/^(VmSize|VmRSS|Threads):/' /proc/$child/status ;;
        phase-start) echo TOP_SOAK_START $(awk '/^CPU:/ {{n++}} END {{print n+0}}' {top_log}) ;;
        phase-end) echo TOP_SOAK_END $(awk '/^CPU:/ {{n++}} END {{print n+0}}' {top_log}) ;;
        load-start*) echo TOP_LOAD_START "$action" $(awk '/^CPU:/ {{n++}} END {{print n+0}}' {top_log}) ;;
        load-end*) echo TOP_LOAD_END "$action" $(awk '/^CPU:/ {{n++}} END {{print n+0}}' {top_log}) ;;
        stop) exit 0 ;;
    esac
done
"""
    ssh = subprocess.Popen(ssh_args(args) + ["-o", "ExitOnForwardFailure=yes", "-L",
                           f"127.0.0.1:{client_port}:127.0.0.1:{remote_port}",
                           args.host, "sh -c " + shlex.quote(remote)],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
    output = []
    ready = threading.Event()

    def read_output():
        for line in ssh.stdout:
            output.append(line.rstrip())
            if line.strip() == "READY":
                ready.set()
    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    client = None
    timings = []
    receive_latency_ms = None
    results = []
    load_results = []
    failure = None
    started = time.monotonic()

    def passed(message):
        results.append(message)
        print("PASS " + message, flush=True)

    def sample():
        ssh.stdin.write("sample\n")
        ssh.stdin.flush()

    def control(action):
        ssh.stdin.write(action + "\n")
        ssh.stdin.flush()

    try:
        print(f"Host RCP listening on {rcp.address}:{rcp.port}; gateway API through SSH localhost forward", flush=True)
        if not ready.wait(12):
            raise RuntimeError("Gateway failed to start: " + "\n".join(output))
        client = Client(client_port, timings)
        assert client.hello[4] == 0
        passed("WebSocket handshake and protocol-v2 unconfigured Hello")
        sample()
        assert client.request(2) == EUI
        passed("GetHwAddress through gateway → host Spinel RCP")
        client.request(0x42, b"\x0f")
        psdu = bytes.fromhex("01087e7d1113f8")
        time.sleep(0.1)
        injected_at = time.perf_counter_ns()
        rcp.inject(psdu)
        received = client.receive()
        receive_latency_ms = (time.perf_counter_ns() - injected_at) / 1e6
        assert received[0] == 0x42 and received[1] & 3 == 2
        assert received[3:] == struct.pack("<BbBH", 15, -42, 200, len(psdu)) + psdu
        passed("Simulated receive → Spinel decoding → client capture event, including escaped bytes")
        client.close()
        client = Client(client_port, timings)
        client.request(0x10, config())
        passed("Configure with synthetic coordinator state")
        client.request(0x16)
        passed("StartNetwork, reset notification and radio property programming")
        info = client.request(0x20)
        assert info[-1] & 1 == 1
        sample()
        # APS broadcasts exercise NWK security, the sender task and completion tracking.
        for _ in range(10):
            broadcast = struct.pack("<BHHHBBBBbH", 0, 0xfffc, 0x104, 6, 1, 1, 0, 3, 0, 4) + b"test"
            client.request(0x31, broadcast)
            time.sleep(0.05)
        passed("Ten APS broadcasts accepted by active stack")
        before_reset = rcp.counts["resets"]
        programmed = rcp.counts["property_32"]
        client.request(0, b"\x01")
        deadline = time.monotonic() + 4
        while rcp.counts["property_32"] <= programmed and time.monotonic() < deadline:
            time.sleep(0.05)
        assert rcp.counts["resets"] > before_reset and rcp.counts["property_32"] > programmed
        # Wait for the last programmed property, not just the first setter.
        while 4869 not in rcp.properties and time.monotonic() < deadline:
            time.sleep(0.05)
        assert rcp.properties[54] == struct.pack("<H", 0x1a62)
        assert rcp.properties[33] == b"\x0f" and rcp.properties[55] == b"\x01"
        passed("Hard reset of simulated RCP and active-stack reprogramming")
        if args.load:
            if args.unlimited_broadcasts:
                name = b"broadcast_token_refill"
                client.request(0x38, bytes([len(name)]) + name + struct.pack("<Q", 0))
                passed("Broadcast limiter disabled only in temporary synthetic test stack")
            from load_rcp import run_load
            run_load(Client, client_port, rcp, rates, args.stage_seconds,
                     timing_summary, control, sample, load_results)
            client.request(0x20)  # Drain this control connection's original confirmations.
        queries = 0
        ssh.stdin.write("phase-start\n")
        ssh.stdin.flush()
        soak_start = time.monotonic()
        while not args.load and time.monotonic() - soak_start < args.seconds:
            client.request(0x20)
            if queries % 20 == 0:
                client.request(0x35, bytes([15 + (queries // 20) % 2]))
                sample()
            queries += 1
            time.sleep(0.02)
        ssh.stdin.write("phase-end\n")
        ssh.stdin.flush()
        assert rcp.counts["tx_frames"] >= 10, rcp.counts
        confirms = [event for event in client.events
                    if event[0] == 0x13 and event[1] & 3 == 3]
        assert len(confirms) == 10 and all(event[3] == 0 for event in confirms), [e.hex() for e in confirms]
        passed("Ten successful asynchronous BroadcastConfirm notifications")
        if not args.load:
            passed(f"{args.seconds}-second active-stack soak, {queries} network queries and channel changes")
        client.close()
        client = Client(client_port, timings)
        assert client.hello[4] & 1
        assert client.request(0x20)[-1] & 1
        passed("Client reconnect preserves configured, running stack")
        client.request(3)
        client.request(0x20, status=0x10)
        passed("Shutdown releases stack; subsequent query reports NotConfigured")
        sample()
        if rcp.error:
            raise RuntimeError(rcp.error)
    except Exception as error:
        failure = f"{type(error).__name__}: {error}"
        print("FAIL " + failure, flush=True)
    finally:
        if client:
            client.close()
        try:
            ssh.stdin.write("stop\n")
            ssh.stdin.flush()
            ssh.wait(timeout=8)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            ssh.terminate()
            try:
                ssh.wait(timeout=3)
            except subprocess.TimeoutExpired:
                ssh.kill()
                ssh.wait()
        reader.join(2)
        rcp.close()
        report = {"passed": results, "failure": failure, "rcp_error": rcp.error,
                  "load_stages": load_results, "unlimited_broadcasts": args.unlimited_broadcasts,
                  "tcp_nodelay": args.tcp_nodelay,
                  "rcp_counts": dict(rcp.counts), "elapsed_seconds": time.monotonic() - started,
                  "remote_log": remote_log, "remote_top_log": top_log,
                  "gateway_output": output, "ssh_exit": ssh.returncode}
        rss = [int(line.split()[1]) for line in output if line.startswith("VmRSS:")]
        cpu = []
        for line in output:
            if "(ziggurat-server) " in line:
                fields = line.split(") ", 1)[1].split()
                cpu.append(int(fields[11]) + int(fields[12]))
        report["resources"] = {"rss_kib_samples": rss, "cpu_ticks_samples": cpu,
                               "cpu_ticks_scope": "main PID only; excludes LinuxThreads workers",
                               "top_interval_seconds": 1,
                               "peak_sampled_rss_kib": max(rss) if rss else None,
                               "sampled_cpu_ticks_delta": cpu[-1] - cpu[0] if cpu else None}
        # top's initial snapshot is not an interval sample.
        samples = top_samples(output)
        report["resources"]["top_process_tree_samples"] = samples
        report["resources"]["top_cpu_mean_percent"] = statistics.mean(
            sample["cpu_percent"] for sample in samples) if samples else None
        report["resources"]["top_cpu_max_percent"] = max(
            (sample["cpu_percent"] for sample in samples), default=None)
        soak_first = next((int(line.split()[1]) for line in output if line.startswith("TOP_SOAK_START ")), None)
        soak_last = next((int(line.split()[1]) for line in output if line.startswith("TOP_SOAK_END ")), None)
        active = [sample["cpu_percent"] for sample in samples
                  if soak_first is not None and soak_last is not None
                  and soak_first + 1 < sample["frame"] <= soak_last]
        report["resources"]["top_soak_cpu_percent"] = {
            "samples": len(active), "mean": statistics.mean(active) if active else None,
            "min": min(active, default=None), "max": max(active, default=None)}
        for index, stage in enumerate(load_results):
            first = next((int(line.split()[-1]) for line in output
                          if line.startswith(f"TOP_LOAD_START load-start {index} ")), None)
            last = next((int(line.split()[-1]) for line in output
                         if line.startswith(f"TOP_LOAD_END load-end {index} ")), None)
            stage_cpu = [entry["cpu_percent"] for entry in samples
                         if first is not None and last is not None and first + 1 < entry["frame"] <= last]
            stage["top_cpu_percent"] = {"samples": len(stage_cpu),
                "mean": statistics.mean(stage_cpu) if stage_cpu else None, "max": max(stage_cpu, default=None)}
            # Resource samples are emitted only on host requests and are deliberately
            # separate from top's shared-address-space process rows.
            start_at = next((pos for pos, line in enumerate(output)
                             if line.startswith(f"TOP_LOAD_START load-start {index} ")), None)
            end_at = next((pos for pos, line in enumerate(output)
                           if line.startswith(f"TOP_LOAD_END load-end {index} ")), None)
            stage_rss = [int(line.split()[1]) for line in output[start_at:end_at]
                         if line.startswith("VmRSS:")] if start_at is not None and end_at is not None else []
            stage["rss_kib_samples"] = stage_rss
            stage["peak_sampled_rss_kib"] = max(stage_rss, default=None)
        report["request_timings"] = timings
        report["latency_by_command"] = {
            f"0x{command:02x}/status0x{status:02x}": timing_summary(
                [entry["elapsed_ms"] for entry in timings
                 if entry["command"] == command and entry["status"] == status])
            for command, status in sorted({(entry["command"], entry["status"]) for entry in timings})}
        report["simulated_rcp_response_ms"] = timing_summary(rcp.response_times_ms)
        report["injected_receive_to_client_ms"] = receive_latency_ms
        (ROOT / "logs").mkdir(exist_ok=True)
        path = ROOT / "logs" / f"simulation-{token}.json"
        path.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Report: {path}", flush=True)
        for line in output:
            print(line)
    return 1 if failure or rcp.error or ssh.returncode else 0


if __name__ == "__main__":
    raise SystemExit(main())
