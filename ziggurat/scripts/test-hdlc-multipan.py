#!/usr/bin/env python3
"""Bounded actual-server test with synthetic Zigbee and Thread-interface traffic.

--host runs the MIPS server in /tmp on gateway; without it runs the host build.
Never opens the physical radio, reads network state or changes services.
"""
import argparse
from collections import deque
import importlib.util
import json
import os
from pathlib import Path
import select
import shlex
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("simulator", ROOT / "ziggurat/scripts/simulate-rcp.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


class HdlcPeer:
    def __init__(self, stream, writer=None):
        self.stream = stream
        self.writer = stream if writer is None else writer
        self.buffer = bytearray()
        self.frames = deque()
        self.tid = 0

    def send(self, data):
        data = sim.encode(data)
        if hasattr(self.writer, "sendall"):
            self.writer.sendall(data)
        else:
            self.writer.write(data)
            self.writer.flush()

    def receive(self):
        end = time.monotonic() + 3
        while not self.frames:
            remaining = end - time.monotonic()
            if remaining <= 0 or not select.select([self.stream], [], [], remaining)[0]:
                raise TimeoutError("Thread endpoint response")
            chunk = self.stream.recv(2048) if hasattr(self.stream, "recv") else os.read(self.stream.fileno(), 2048)
            if not chunk:
                raise RuntimeError("Thread endpoint closed")
            self.buffer.extend(chunk)
            while 0x7e in self.buffer:
                index = self.buffer.index(0x7e)
                raw = bytes(self.buffer[:index])
                del self.buffer[:index + 1]
                if raw:
                    self.frames.append(sim.decode(raw))
        return self.frames.popleft()

    def request(self, command, prop, value=b""):
        self.tid = self.tid % 15 + 1
        self.send(bytes([0x80 | self.tid, command]) + sim.packed(prop) + value)
        while True:
            frame = self.receive()
            if frame[0] & 15 == self.tid:
                assert frame[0] >> 4 & 3 == 0
                assert frame[1] == 6
                actual, offset = sim.unpacked(frame, 2)
                return actual, frame[offset:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host")
    parser.add_argument("--password-file", default=str(ROOT.parent / "ssh_password.txt"))
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    token = str(time.time_ns())
    device = args.host or "127.0.0.1"
    radio = sim.Rcp(device, tcp_nodelay=True, iid=1)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    api_port = listener.getsockname()[1]
    listener.close()
    thread_path = f"/tmp/ziggurat-hdlc-{token}.sock"
    process = bridge = api = thread_socket = None
    output = []
    report = {"target": device, "results": [], "thread_query_ms": []}
    ssh_base = sim.ssh_args(args) if args.host else []
    log = ROOT / "logs" / f"hdlc-multipan-{token}.log"
    log.parent.mkdir(exist_ok=True)
    log_file = log.open("w")

    def passed(message):
        report["results"].append(message)
        print("PASS", message, flush=True)

    def upload(path, remote):
        with path.open("rb") as source:
            subprocess.run(ssh_base + [f"root@{args.host}",
                f"umask 077; test ! -e {shlex.quote(remote)} && cat > {shlex.quote(remote)} && chmod 700 {shlex.quote(remote)}"],
                stdin=source, check=True, timeout=40)

    try:
        if args.host:
            binary = args.binary or ROOT / "build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped"
            remote_binary = f"/tmp/ziggurat-hdlc-{token}"
            remote_probe = remote_binary + "-probe"
            # This gateway uses ramfs: its file cache is not safely reclaimable.
            # Require genuinely free RAM, leaving 8 MiB beyond uploaded file sizes.
            memory = subprocess.check_output(ssh_base + [f"root@{args.host}", "cat /proc/meminfo"],
                text=True, timeout=12)
            values = {line.split(":")[0]: int(line.split()[1]) for line in memory.splitlines() if line.strip()}
            available = values.get("MemFree", 0)
            required = (binary.stat().st_size + (ROOT / "build/hdlc-unix-probe").stat().st_size + 1023) // 1024 + 8192
            report["memory_preflight_kib"] = {"available": available, "required": required}
            if available < required:
                raise RuntimeError(f"Insufficient gateway memory for RAM-backed uploads: {available} KiB available, {required} required")
            upload(binary, remote_binary)
            upload(ROOT / "build/hdlc-unix-probe", remote_probe)
            remote_port = 19996
            command = f"""set -u
ulimit -c 0
{remote_binary} --device tcp://{radio.address}:{radio.port} --spinel-iid 1 --thread-socket {thread_path} --listen 127.0.0.1:{remote_port} --log-level info > {remote_binary}.log 2>&1 &
child=$!
(sleep 45; kill -9 "$child" 2>/dev/null) &
watchdog=$!
cleanup() {{ kill "$child" "$watchdog" 2>/dev/null; wait "$child" 2>/dev/null; wait "$watchdog" 2>/dev/null; cat {remote_binary}.log; rm -f {thread_path} {remote_binary} {remote_probe} {remote_binary}.log; }}
trap cleanup EXIT HUP INT TERM
sleep 1
kill -0 "$child" || exit 1
echo READY
while read action; do
 case "$action" in
  sample) awk '/^(VmSize|VmRSS|Threads):/' /proc/$child/status ;;
  stop) exit 0 ;;
 esac
done
"""
            process = subprocess.Popen(ssh_base + ["-o", "ExitOnForwardFailure=yes", "-L",
                f"127.0.0.1:{api_port}:127.0.0.1:{remote_port}", f"root@{args.host}", "sh -c " + shlex.quote(command)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            ready = threading.Event()

            def read_output():
                for line in process.stdout:
                    output.append(line.rstrip())
                    if line.strip() == "READY": ready.set()

            threading.Thread(target=read_output, daemon=True).start()
            if not ready.wait(12): raise RuntimeError("Gateway startup failed: " + "\n".join(output))
            bridge = subprocess.Popen(ssh_base + [f"root@{args.host}", remote_probe, thread_path],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log_file)
            thread = HdlcPeer(bridge.stdout, bridge.stdin)
        else:
            binary = args.binary or ROOT / "build/host/debug/ziggurat"
            process = subprocess.Popen([str(binary), "--device", f"tcp://{radio.address}:{radio.port}",
                "--spinel-iid", "1", "--thread-socket", thread_path, "--listen", f"127.0.0.1:{api_port}",
                "--log-level", "info"], stdout=log_file, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 5
            while not Path(thread_path).exists():
                if process.poll() is not None or time.monotonic() > deadline: raise RuntimeError("Host startup failed")
                time.sleep(0.02)
            thread_socket = socket.socket(socket.AF_UNIX)
            thread_socket.connect(thread_path)
            thread = HdlcPeer(thread_socket)
        api = sim.Client(api_port, [])
        assert api.request(2) == sim.EUI
        passed("Zigbee API uses IID 1 through actual server")
        assert thread.request(2, 8) == (8, sim.EUI[::-1])
        passed("Local Thread HDLC consumer IID 0 maps to RCP IID 2")
        api.request(0x10, sim.config())
        api.request(0x16)
        deadline = time.monotonic() + 3
        while 54 not in radio.interfaces[1] and time.monotonic() < deadline: time.sleep(0.01)
        assert radio.interfaces[1][54] == struct.pack("<H", 0x1a62)
        passed("Zigbee starts with synthetic state and owner reset")
        for prop, value in [(32, b"\x01"), (33, b"\x0f"), (54, struct.pack("<H", 0x2b73)),
            (53, struct.pack("<H", 0x5678)), (4867, b"\x01"), (4868, struct.pack("<H", 0x1234))]:
            assert thread.request(3, prop, value) == (prop, value)
        assert radio.interfaces[1][54] != radio.interfaces[2][54]
        assert radio.interfaces[1][53] != radio.interfaces[2][53]
        passed("PAN, short address and source-match configuration stay separate")
        before = radio.counts["resets"]
        thread.tid = thread.tid % 15 + 1
        thread.send(bytes([0x80 | thread.tid, 1, 2]))
        while True:
            reply = thread.receive()
            if reply[0] & 15 == thread.tid: break
        assert reply[2:] == b"\x00\x04"
        assert radio.counts["resets"] == before
        assert thread.request(3, 33, b"\x14") == (0, b"\x03")
        assert radio.interfaces[2][33] == b"\x0f"
        passed("Secondary reset and conflicting channel are rejected before reaching RCP")
        raw = bytes.fromhex("030801ffffffffff07")
        radio.inject(raw, iid=2)
        while True:
            frame = thread.receive()
            prop, offset = sim.unpacked(frame, 2)
            if prop == 113: break
        assert frame[0] == 0x80 and raw in frame[offset:]
        passed("Thread raw receive reaches local consumer with IID translation")
        for _ in range(30):
            start = time.perf_counter_ns()
            assert thread.request(2, 54) == (54, struct.pack("<H", 0x2b73))
            report["thread_query_ms"].append((time.perf_counter_ns() - start) / 1e6)
            api.request(0x20)
        assert radio.interfaces[1][54] == struct.pack("<H", 0x1a62)
        passed("Thirty mixed Thread queries and Zigbee NetworkInfo requests complete")
        programmed = radio.counts["iid_1_property_32"]
        radio.reset_properties()
        radio.send(0x80, 0, sim.packed(114))
        while True:
            frame = thread.receive()
            if frame[2:] == b"\x00\x72": break
        deadline = time.monotonic() + 3
        while (radio.counts["iid_1_property_32"] <= programmed or 4869 not in radio.interfaces[1]) and time.monotonic() < deadline: time.sleep(0.01)
        assert radio.counts["iid_1_property_32"] > programmed
        assert radio.interfaces[1][54] == struct.pack("<H", 0x1a62)
        passed("Global reset notifies Thread consumer and triggers Zigbee recovery")
        report["passed"] = True
        report["thread_query_timing"] = sim.timing_summary(report["thread_query_ms"])
        if args.host:
            process.stdin.write("sample\n"); process.stdin.flush(); time.sleep(0.1)
    finally:
        if api: api.close()
        if thread_socket: thread_socket.close()
        if bridge:
            bridge.stdin.close()
            bridge.wait(timeout=5)
        if process:
            if args.host:
                process.stdin.write("stop\n"); process.stdin.flush()
            else: process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        if args.host:
            try:
                subprocess.run(ssh_base + [f"root@{args.host}",
                    f"rm -f /tmp/ziggurat-hdlc-{token} /tmp/ziggurat-hdlc-{token}-probe /tmp/ziggurat-hdlc-{token}.log {thread_path}"],
                    check=True, timeout=10, stdout=subprocess.DEVNULL)
            except (subprocess.SubprocessError, OSError) as error:
                report["cleanup_error"] = str(error)
        radio.close()
        report["counts"] = dict(radio.counts)
        report["gateway_output"] = output
        report["simulator_error"] = radio.error
        log_file.close()
        if not args.host: Path(thread_path).unlink(missing_ok=True)
        path = ROOT / "logs" / f"hdlc-multipan-{token}.json"
        path.write_text(json.dumps(report, indent=2) + "\n")
        print("Report:", path, flush=True)
    assert report.get("passed") and not radio.error


if __name__ == "__main__": main()
