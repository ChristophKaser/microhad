#!/usr/bin/env python3
"""Host-only runtime check: synthetic RCP, private state, no UART or deployment."""
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ot_sim", ROOT / "scripts/test-openthread-device.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def receive(peer, length):
    result = bytearray()
    while len(result) < length:
        data = peer.recv(length - len(result))
        if not data:
            raise RuntimeError("Service closed a valid request")
        result.extend(data)
    return result


def status(path, opcode, ident):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(1)
        peer.connect(str(path))
        request = struct.pack("<HBBBBI", 0, 1, 0, opcode, 0, ident)
        peer.sendall(request[:3])
        peer.sendall(request[3:])
        header = receive(peer, 10)
        length, version, kind, operation, reserved, response_id = struct.unpack("<HBBBBI", header)
        assert (version, kind, operation, reserved, response_id) == (1, 1, opcode, 0, ident)
        assert length <= 2048
        return json.loads(receive(peer, length))


def main():
    radio = sim.ThreadRcp("127.0.0.1", tcp_nodelay=True)
    relay = service = None
    try:
        with tempfile.TemporaryDirectory(prefix="ot-sim-service-") as directory:
            private = Path(directory)
            radio_path = private / "radio.sock"
            ipc_path = private / "matter.sock"
            relay = subprocess.Popen([str(ROOT / "build/openthread-unix/sim-relay-host"),
                str(radio_path), radio.address, str(radio.port), "30"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 3
            while not radio_path.exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Synthetic relay did not start")
                time.sleep(0.02)
            args = [str(ROOT / "build/rs-matter/target/release/thread-matter"),
                "--socket", str(ipc_path), "--state-dir", str(private / "state"),
                "--radio-url", f"spinel+hdlc+unix://{radio_path}?no-reset"]
            service = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5
            while True:
                if service.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Service did not become responsive")
                try:
                    matter = status(ipc_path, 1, 301)
                    break
                except (OSError, RuntimeError):
                    time.sleep(0.02)
            thread = status(ipc_path, 2, 302)
            assert matter["available"] and matter["state"] == "awaiting_fabric"
            assert not matter["fabric_ready"]
            assert thread["available"] and not thread["attached"]
            assert not thread["linux_ipv6_or_tun"]
            assert ipc_path.stat().st_mode & 0o777 == 0o600
            duplicate = subprocess.run(args, capture_output=True, timeout=2)
            assert duplicate.returncode == 1
            assert status(ipc_path, 1, 303)["available"]
            # Status traffic stays responsive across multiple independent clients.
            start = time.monotonic()
            for ident in range(400, 420):
                assert status(ipc_path, ident % 2 + 1, ident)["available"]
            assert time.monotonic() - start < 2
            service.terminate()
            stdout, stderr = service.communicate(timeout=2)
            assert service.returncode == 0
            assert not ipc_path.exists()
            assert not stdout
            assert stderr == b"Matter controller service started\nMatter controller service stopped\n"
            assert not radio.error
            assert radio.counts.get("command_1", 0) == 0
            print("PASS status, duplicate start, private socket, bounded calls, SIGTERM, no RCP reset")
    finally:
        for process in (service, relay):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
        radio.close()


if __name__ == "__main__":
    main()
