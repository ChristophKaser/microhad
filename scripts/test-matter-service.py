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

class ChannelRcp(sim.ThreadRcp):
    def reset_properties(self):
        super().reset_properties()
        self.channel_writes = []
    def receive_frame(self, data):
        command, offset = sim.sim.unpacked(data, 1)
        prop, offset = sim.sim.unpacked(data, offset)
        if command == 3 and prop == 33:
            assert len(data[offset:]) == 1
            self.channel_writes.append(data[offset])
        super().receive_frame(data)


def receive(peer, length):
    result = bytearray()
    while len(result) < length:
        data = peer.recv(length - len(result))
        if not data:
            raise RuntimeError("Service closed a valid request")
        result.extend(data)
    return result


def status(path, opcode, ident, body=b""):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(1)
        peer.connect(str(path))
        request = struct.pack("<HBBBBI", len(body), 1, 0, opcode, 0, ident) + body
        peer.sendall(request[:3])
        peer.sendall(request[3:])
        header = receive(peer, 10)
        length, version, kind, operation, reserved, response_id = struct.unpack("<HBBBBI", header)
        assert (version, kind, operation, reserved, response_id) == (1, 1, opcode, 0, ident)
        assert length <= 2048
        return json.loads(receive(peer, length))


def ipc_frame(peer):
    header = receive(peer, 10)
    length, version, kind, opcode, reserved, ident = struct.unpack("<HBBBBI", header)
    assert version == 1 and reserved == 0 and length <= 2048
    return kind, opcode, ident, json.loads(receive(peer, length))


class SessionPeer:
    def __init__(self, path):
        self.peer = socket.socket(socket.AF_UNIX)
        self.peer.settimeout(1)
        self.peer.connect(str(path))
        self.ident = 1000
        self.events = []

    def call(self, opcode, body):
        self.ident += 1
        body = json.dumps(body).encode() if body is not None else b""
        self.peer.sendall(struct.pack("<HBBBBI", len(body), 1, 0, opcode, 0, self.ident) + body)
        while True:
            kind, operation, ident, body = ipc_frame(self.peer)
            if kind == 2:
                assert operation == 12 and ident == 0 and body["schema"] == 1
                assert len(self.events) < 128
                self.events.append(body)
                continue
            assert (kind, operation, ident) == (1, opcode, self.ident)
            return body

    def event(self, name, generation):
        while True:
            for index, event in enumerate(self.events):
                if event["event"] == name and event["data"]["generation"] == generation:
                    return self.events.pop(index)["data"]
            kind, opcode, ident, body = ipc_frame(self.peer)
            assert (kind, opcode, ident) == (2, 12, 0)
            self.events.append(body)

    def close(self):
        self.peer.close()


def commissioning_calls(path, credentials):
    client = SessionPeer(path)
    try:
        start = dict(schema=1, discriminator=3840, pin=20202021, timeout_seconds=30)
        heartbeat = dict(schema=1, action="observe", zigbee_channel=15, zigbee_online=True)
        assert client.call(9, heartbeat)["ok"]
        before = credentials.read_bytes()
        first = client.call(3, start)
        assert first["ok"] and first["result"]["state"] == "waiting_ble"
        session = first["result"]["session"]
        assert len(session) == 16 and int(session, 16) != 0
        assert "20202021" not in json.dumps(first)
        assert client.call(3, start)["error"] == "busy"
        state = client.call(4, dict(schema=1, session=session, subscribe=True))
        assert state["result"]["state"] == "waiting_ble"
        assert credentials.read_bytes() == before
        attach = dict(schema=1, session=session, address="bbbbbbbbbbbb", mtu=247)
        generation = client.call(6, attach)["result"]["generation"]
        write = client.event("ble_write", generation)
        assert write["session"] == session and write["write"] > 0
        assert 1 <= len(bytes.fromhex(write["data"])) <= 244
        assert credentials.read_bytes() != before  # allocator committed before actual transport work
        assert client.call(7, dict(schema=1, session=session, generation=generation+1,
            sequence=1, data="01"))["error"] == "stale_generation"
        assert client.call(7, dict(schema=1, session=session, generation=generation,
            sequence=2, data="01"))["error"] == "stale_generation"
        assert client.call(8, dict(schema=1, session=session, generation=generation,
            write=write["write"]+1, success=True))["error"] == "stale_generation"
        # Replacing PASE drops the actual task and its authority borrow before starting another.
        newer = client.call(6, attach)["result"]["generation"]
        assert newer > generation
        new_write = client.event("ble_write", newer)
        assert client.call(8, dict(schema=1, session=session, generation=generation,
            write=write["write"], success=True))["error"] == "stale_generation"
        cancelled = client.call(5, dict(schema=1, session=session))
        assert cancelled["result"]["state"] == "cancelled"
        assert cancelled["result"]["deadline_remaining_seconds"] == 0
        assert client.call(5, dict(schema=1, session=session)) == cancelled
        assert client.call(8, dict(schema=1, session=session, generation=newer,
            write=new_write["write"], success=True))["error"] == "cancelled"
        assert client.call(9, heartbeat)["ok"]
        second = client.call(3, start)["result"]["session"]
        assert second != session
        assert client.call(4, dict(schema=1, session=session))["error"] == "stale_session"
        attach["session"] = second
        generation = client.call(6, attach)["result"]["generation"]
        write = client.event("ble_write", generation)
        assert client.call(8, dict(schema=1, session=second, generation=generation,
            write=write["write"], success=False))["error"] == "transport"
        failed = client.call(4, dict(schema=1, session=second))
        assert failed["result"]["state"] == "failed" and failed["result"]["error"] == "transport"
        # Abandoned waiting-BLE jobs expire without status/heartbeat extending the deadline.
        client.call(9, heartbeat)
        expired_session = client.call(3, start)["result"]["session"]
        before = credentials.read_bytes()
        deadline = time.monotonic() + 33
        while True:
            client.call(9, heartbeat)
            state = client.call(4, dict(schema=1, session=expired_session))
            if state["result"]["state"] == "timed_out":
                break
            assert state["result"]["state"] == "waiting_ble" and time.monotonic() < deadline
            client.events.clear()
            time.sleep(.5)
        assert state["result"]["error"] == "timeout"
        assert state["result"]["deadline_remaining_seconds"] == 0
        assert credentials.read_bytes() == before
    finally:
        client.close()


def main():
    radio = ChannelRcp("127.0.0.1", tcp_nodelay=True)
    relay = service = None
    try:
        with tempfile.TemporaryDirectory(prefix="ot-sim-service-") as directory:
            private = Path(directory)
            radio_path = private / "radio.sock"
            ipc_path = private / "matter.sock"
            relay = subprocess.Popen([str(ROOT / "build/openthread-unix/sim-relay-host"),
                str(radio_path), radio.address, str(radio.port), "60"],
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
            assert matter["available"] and matter["state"] == "awaiting_coordinator"
            assert matter["fabric_ready"]
            assert thread["available"] and not thread["attached"]
            assert not thread["linux_ipv6_or_tun"]
            assert thread["state"] == "awaiting_coordinator"
            assert not radio.requests
            subscriber = socket.socket(socket.AF_UNIX)
            subscriber.settimeout(1)
            subscriber.connect(str(ipc_path))
            body = b'{"schema":1,"action":"status","subscribe":true}'
            subscriber.sendall(struct.pack("<HBBBBI", len(body), 1, 0, 9, 0, 307) + body)
            kind, opcode, ident, body = ipc_frame(subscriber)
            assert (kind, opcode, ident) == (1, 9, 307) and body["ok"]
            kind, opcode, ident, body = ipc_frame(subscriber)
            assert (kind, opcode, ident) == (2, 12, 0)
            assert body["schema"] == 1 and body["event"] == "thread"
            assert body["data"]["state"] == "awaiting_coordinator"
            assert not ({"dataset", "pin", "key"} & body["data"].keys())
            observed = status(ipc_path, 9, 310, b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')
            assert observed["ok"]
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 311)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(0.01)
            kind, opcode, ident, body = ipc_frame(subscriber)
            assert (kind, opcode, ident) == (2, 12, 0) and body["event"] == "thread"
            assert body["data"]["zigbee_channel"] == 15
            assert body["data"]["state"] == "detached"
            subscriber.close()
            assert ipc_path.stat().st_mode & 0o777 == 0o600
            duplicate = subprocess.run(args, capture_output=True, timeout=2)
            assert duplicate.returncode == 1
            assert status(ipc_path, 1, 303)["available"]
            reserved = status(ipc_path, 9, 304, b'{"schema":1,"action":"status"}')
            assert reserved["ok"] and not reserved["result"]["configured"]
            malformed = status(ipc_path, 9, 305, b'{"schema":1,"action":"status","secret":"must-not-log"}')
            assert malformed == {"schema": 1, "ok": False, "error": "malformed"}
            credentials = private / "state/credentials.bin"
            before = credentials.read_bytes()
            calls = len(radio.requests)
            invalid = status(ipc_path, 9, 312, b'{"schema":1,"action":"import","zigbee_channel":15,"dataset":"01"}')
            assert invalid["error"] == "malformed"
            mismatch = status(ipc_path, 9, 313, b'{"schema":1,"action":"create","zigbee_channel":11}')
            assert mismatch["error"] == "not_ready"
            assert credentials.read_bytes() == before
            assert len(radio.requests) == calls
            created = status(ipc_path, 9, 314, b'{"schema":1,"action":"create","zigbee_channel":15}')
            assert created["ok"] and created["result"]["configured"]
            assert created["result"]["state"] in ("attaching", "attached")
            assert "dataset" not in created["result"]
            commissioning_calls(ipc_path, credentials)
            # Status traffic stays responsive across multiple independent clients.
            start = time.monotonic()
            for ident in range(400, 420):
                assert status(ipc_path, ident % 2 + 1, ident)["available"]
            assert time.monotonic() - start < 2
            committed = credentials.read_bytes()
            metadata = credentials.stat()
            time.sleep(5.1)
            expired = status(ipc_path, 2, 499)
            assert expired["state"] == "awaiting_coordinator" and not expired["attached"]
            assert credentials.read_bytes() == committed
            service.terminate()
            stdout, stderr = service.communicate(timeout=2)
            assert service.returncode == 0
            assert not ipc_path.exists()
            assert not stdout
            assert stderr == b"Matter controller service started\nMatter controller service stopped\n"
            assert not radio.error
            assert radio.counts.get("command_1", 0) == 0
            assert radio.channel_writes and all(channel == 15 for channel in radio.channel_writes)
            relay.wait(timeout=2)
            radio.close()
            # Corrupt only the cached active channel (valid POSIX record envelope).
            # Guarded OT startup must ignore it and apply the authoritative dataset.
            changed = False
            for path in (private / "state/tmp").glob("*.data"):
                blob = bytearray(path.read_bytes())
                offset = 0
                while offset < len(blob):
                    key, length = struct.unpack_from("=HH", blob, offset)
                    if key == 1:
                        value = bytes(blob[offset+4:offset+4+length])
                        old = b'\x00\x03\x00\x00\x0f'
                        if old not in value:
                            offset += 4 + length
                            continue
                        blob[offset+4:offset+4+length] = value.replace(old, b'\x00\x03\x00\x00\x14', 1)
                        changed = True
                    offset += 4 + length
                path.write_bytes(blob)
            assert changed
            radio = ChannelRcp("127.0.0.1", tcp_nodelay=True)
            relay = subprocess.Popen([str(ROOT / "build/openthread-unix/sim-relay-host"),
                str(radio_path), radio.address, str(radio.port), "30"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 3
            while not radio_path.exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Restart relay did not start")
                time.sleep(0.02)
            service = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5
            while True:
                if service.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Service did not restore fabric")
                try:
                    restored = status(ipc_path, 1, 701)
                    break
                except (OSError, RuntimeError):
                    time.sleep(0.02)
            assert restored["fabric_ready"]
            assert not radio.requests
            mismatch = status(ipc_path, 9, 702, b'{"schema":1,"action":"observe","zigbee_channel":11,"zigbee_online":true}')
            assert mismatch["error"] == "not_ready"
            assert status(ipc_path, 2, 703)["state"] == "channel_mismatch"
            assert not radio.requests
            observed = status(ipc_path, 9, 704, b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')
            assert observed["ok"]
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 705)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(0.01)
            assert status(ipc_path, 2, 706)["configured"]
            assert credentials.read_bytes() == committed
            assert (credentials.stat().st_ino, credentials.stat().st_mtime_ns) == (metadata.st_ino, metadata.st_mtime_ns)
            service.terminate()
            stdout, stderr = service.communicate(timeout=2)
            assert service.returncode == 0
            assert not stdout
            assert stderr == b"Matter controller service started\nMatter controller service stopped\n"
            wrong_vendor = subprocess.run(args + ["--administrator-vendor-id", "65522"], capture_output=True, timeout=2)
            assert wrong_vendor.returncode == 1
            assert wrong_vendor.stderr == b"Matter controller service failed\n"
            assert credentials.read_bytes() == committed
            assert not ipc_path.exists()
            # Simulate a corrupt committed envelope only in isolated temporary state.
            credentials.write_bytes(committed[:8])
            corrupt = subprocess.run(args, capture_output=True, timeout=2)
            assert corrupt.returncode == 1
            assert corrupt.stderr == b"Matter controller service failed\n"
            assert credentials.read_bytes() == committed[:8]
            assert not ipc_path.exists()
            assert not radio.error
            assert radio.counts.get("command_1", 0) == 0
            assert radio.channel_writes and all(channel == 15 for channel in radio.channel_writes)
            print("PASS commissioner IPC/BTP lifetime and timeout, targeted Thread events, fabric restore, gated Thread create/restore, mismatch/expiry/cache guards, no read writes, no RCP reset")
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
