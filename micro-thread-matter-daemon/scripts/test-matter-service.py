#!/usr/bin/env python3
"""Host-only runtime check: synthetic RCP, private state, no UART or deployment."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import shutil
import stat
import tarfile
import tempfile
import threading
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import matter_wire

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE_HELPER = ROOT / "microhad/scripts/matter_archive.py"
spec = importlib.util.spec_from_file_location("ot_sim", ROOT / "micro-thread-matter-daemon/scripts/test-openthread-device.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)

class ChannelRcp(sim.ThreadRcp):
    def reset_properties(self):
        super().reset_properties()
        self.channel_writes = []
        self.dataset_write_count = 0
        self.dataset_write_errors = 0
        self.fail_next_dataset_write = False
        self.pause_dataset_write = False
        self.dataset_write_seen = threading.Event()
        self.release_dataset_write = threading.Event()
    def receive_frame(self, data):
        command, offset = sim.sim.unpacked(data, 1)
        prop, offset = sim.sim.unpacked(data, offset)
        if command == 3 and prop == 33:
            assert len(data[offset:]) == 1
            self.channel_writes.append(data[offset])
        if command == 3 and prop == 2048:
            self.dataset_write_count += 1
            if self.pause_dataset_write:
                self.dataset_write_seen.set()
                self.release_dataset_write.wait(timeout=3)
            if self.fail_next_dataset_write:
                self.fail_next_dataset_write = False
                self.dataset_write_errors += 1
                self.send(data[0], 0, sim.sim.packed(2))
                return
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
        request = matter_wire.frame(opcode, 0, ident, body)
        peer.sendall(request[:3]); peer.sendall(request[3:])
        kind, operation, response_id, result = ipc_frame(peer)
        assert (kind, operation, response_id) == (1, opcode, ident)
        return result


def ipc_frame(peer):
    return matter_wire.receive(peer, receive)


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
        self.peer.sendall(matter_wire.frame(opcode, 0, self.ident, body))
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


def catalog_records(credentials):
    # Independent decoder for the private envelope and public node catalog fields.
    data = credentials.read_bytes()
    assert data[:8] == b"TMKV\x01\0\0\0"
    count = struct.unpack_from("<H", data, 16)[0]
    offset = 18
    catalog = None
    for _ in range(count):
        key, length = struct.unpack_from("<HH", data, offset)
        offset += 4
        value = data[offset:offset + length]
        assert len(value) == length
        offset += length
        if key == 0x1002:
            catalog = value
    assert offset == len(data) and catalog is not None
    assert catalog[:8] == b"TMNC\x01\0\0\0"
    assert struct.unpack_from("<Q", catalog, 8)[0] != 0
    count = struct.unpack_from("<H", catalog, 16)[0]
    assert count <= 64 and len(catalog) == 18 + count * 9
    return [struct.unpack_from("<QB", catalog, 18 + index * 9) for index in range(count)]


def thread_dataset(credentials):
    data = credentials.read_bytes()
    assert data[:8] == b"TMKV\x01\0\0\0"
    count = struct.unpack_from("<H", data, 16)[0]
    offset = 18
    for _ in range(count):
        key, length = struct.unpack_from("<HH", data, offset)
        offset += 4
        value = data[offset:offset + length]
        assert len(value) == length
        offset += length
        if key == 0x1001:
            assert value[:8] == b"TMDT\x01\0\0\0"
            size = struct.unpack_from("<H", value, 9)[0]
            assert len(value) == 11 + size
            return value[11:]
    raise AssertionError("Synthetic Thread dataset missing")


def state_image(directory):
    image = {}
    for path in sorted(directory.rglob("*")):
        metadata = path.lstat()
        relative = str(path.relative_to(directory))
        contents = path.read_bytes() if stat.S_ISREG(metadata.st_mode) else None
        image[relative] = (metadata.st_mode & 0o777, metadata.st_ino,
                           metadata.st_mtime_ns, metadata.st_size, contents)
    return image


def archive_state(source, archive_path):
    with tarfile.open(archive_path, "w:", format=tarfile.USTAR_FORMAT) as archive:
        for path in sorted(source.iterdir()):
            archive.add(path, arcname=path.name)


def commissioning_calls(path, credentials):
    client = SessionPeer(path)
    try:
        start = dict(schema=1, discriminator=3840, pin=20202021, timeout_seconds=30,
                     country_code="DE", regulatory_location="indoor")
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
        records = catalog_records(credentials)
        assert len(records) == 1 and records[0][0] >= 2 and records[0][1] == 1
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
        records = catalog_records(credentials)
        assert len(records) == 2 and records[0][0] < records[1][0]
        assert all(phase == 5 for _, phase in records)
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
        records = catalog_records(credentials)
        assert len(records) == 3 and all(phase == 5 for _, phase in records)
        assert len(set(node for node, _ in records)) == 3

        # Losing the coordinator while a phone relay is active must fail the
        # commissioning task and invalidate its outstanding BLE write.
        assert client.call(9, heartbeat)["ok"]
        active_session = client.call(3, start)["result"]["session"]
        attach["session"] = active_session
        generation = client.call(6, attach)["result"]["generation"]
        write = client.event("ble_write", generation)
        assert write["session"] == active_session
        assert client.call(9, dict(schema=1, action="observe", zigbee_channel=0,
                                   zigbee_online=False))["ok"]
        deadline = time.monotonic() + 3
        while True:
            failed = client.call(4, dict(schema=1, session=active_session))
            if failed["result"]["state"] == "failed":
                break
            assert time.monotonic() < deadline, failed
            time.sleep(.01)
        assert failed["result"]["error"] == "not_ready"
        late_write = client.call(8, dict(schema=1, session=active_session,
            generation=generation, write=write["write"], success=True))
        assert late_write.get("error") in ("cancelled", "stale_generation", "stale_session"), late_write
        assert client.call(9, heartbeat)["ok"]

        # If the IPC peer disappears before the native side receives or can
        # cancel the session, the Rust owner retires it instead of staying busy.
        orphaned = client.call(3, start)["result"]["session"]
        client.close()
        client = SessionPeer(path)
        deadline = time.monotonic() + 3
        while True:
            state = client.call(4, dict(schema=1, session=orphaned))
            if state["result"]["state"] == "cancelled":
                break
            assert time.monotonic() < deadline, state
            time.sleep(.01)
        assert state["result"]["error"] == "cancelled"

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
            args = [str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter"),
                "--socket", str(ipc_path), "--state-dir", str(private / "state"),
                "--radio-url", f"spinel+hdlc+unix://{radio_path}?no-reset"]
            args += ["--log-level", os.environ.get("MICROHAD_MATTER_TEST_LOG_LEVEL", "info")]
            if os.environ.get("MICROHAD_MATTER_TEST_RAW_FRAMES") == "on":
                args.append("--raw-frames")
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
            page = status(ipc_path, 10, 401, b'{"schema":1,"action":"snapshot"}')
            assert page["ok"] and page["result"]["devices"] == []
            assert page["result"]["next_cursor"] is None
            revision = page["result"]["revision"]
            assert len(revision) == 16 and int(revision, 16) != 0
            stale = status(ipc_path, 10, 402, b'{"schema":1,"action":"snapshot","cursor":1}')
            assert stale["error"] == "stale_revision"
            malformed = status(ipc_path, 10, 403, json.dumps({"schema": 1, "action": "snapshot",
                "cursor": 65535, "revision": revision}).encode())
            assert malformed["error"] == "malformed"
            assert thread["available"] and not thread["attached"]
            assert not thread["linux_ipv6_or_tun"]
            assert thread["state"] == "awaiting_coordinator"
            assert not radio.requests
            subscriber = socket.socket(socket.AF_UNIX)
            subscriber.settimeout(1)
            subscriber.connect(str(ipc_path))
            body = b'{"schema":1,"action":"status","subscribe":true}'
            subscriber.sendall(matter_wire.frame(9, 0, 307, body))
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
            assert malformed["ok"] and "secret" not in malformed["result"]
            credentials = private / "state/credentials.bin"
            before = credentials.read_bytes()
            calls = len(radio.requests)
            invalid = status(ipc_path, 9, 312, b'{"schema":1,"action":"import","zigbee_channel":15,"dataset":"01"}')
            assert invalid["error"] == "malformed"
            mismatch = status(ipc_path, 9, 313, b'{"schema":1,"action":"create","zigbee_channel":11}')
            assert mismatch["error"] == "not_ready"
            assert credentials.read_bytes() == before
            assert len(radio.requests) == calls

            apply_result = []
            radio.pause_dataset_write = True
            def create_during_apply():
                try:
                    apply_result.append(status(ipc_path, 9, 314,
                        b'{"schema":1,"action":"create","zigbee_channel":15}'))
                except Exception as error:
                    apply_result.append(error)
            apply_request = threading.Thread(target=create_during_apply, daemon=True)
            apply_request.start()
            assert radio.dataset_write_seen.wait(timeout=3), "Dataset apply never reached the RCP"
            interrupted_dataset = thread_dataset(credentials)
            interrupted_envelope = credentials.read_bytes()
            assert interrupted_dataset and interrupted_envelope != before
            relay.terminate()
            relay.wait(timeout=2)
            radio.release_dataset_write.set()
            apply_request.join(timeout=5)
            assert not apply_request.is_alive()
            assert apply_result and isinstance(apply_result[0], Exception), apply_result
            assert service.wait(timeout=5) != 0
            service_output, service_error = service.communicate(timeout=2)
            # Lifecycle messages go to syslog; the host service has no stderr logger.
            assert not service_output and not service_error
            assert credentials.read_bytes() == interrupted_envelope
            assert thread_dataset(credentials) == interrupted_dataset

            # The interrupted apply committed its dataset before touching the
            # RCP. Restart must wait for a fresh coordinator observation, then
            # restore only that persisted dataset on the permitted channel.
            radio.close()
            if radio_path.exists() and stat.S_ISSOCK(radio_path.lstat().st_mode):
                radio_path.unlink()
            radio = ChannelRcp("127.0.0.1", tcp_nodelay=True)
            relay = subprocess.Popen([str(ROOT / "build/openthread-unix/sim-relay-host"),
                str(radio_path), radio.address, str(radio.port), "60"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 3
            while not radio_path.exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Recovery relay did not start")
                time.sleep(0.02)
            service = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5
            while True:
                if service.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Service did not wait after interrupted apply")
                try:
                    assert status(ipc_path, 2, 315)["state"] == "awaiting_coordinator"
                    break
                except (OSError, RuntimeError):
                    time.sleep(0.02)
            assert not radio.requests
            assert credentials.read_bytes() == interrupted_envelope
            assert thread_dataset(credentials) == interrupted_dataset
            resumed = status(ipc_path, 9, 316,
                b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')
            assert resumed["ok"]
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 317)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(0.01)
            assert radio.channel_writes and all(channel == 15 for channel in radio.channel_writes)
            created = status(ipc_path, 2, 318)
            assert created["configured"] and created["dataset_channel"] == 15
            assert credentials.read_bytes() == interrupted_envelope
            commissioning_calls(ipc_path, credentials)

            # RCP socket death must stop the service; restored state must wait
            # for a fresh matching coordinator observation before radio use.
            previous_nodes = catalog_records(credentials)
            previous_dataset = thread_dataset(credentials)
            relay_client = SessionPeer(ipc_path)
            active_session = relay_client.call(3, dict(
                schema=1, discriminator=3840, pin=20202021, timeout_seconds=30,
                country_code="DE", regulatory_location="indoor"))["result"]["session"]
            generation = relay_client.call(6, dict(schema=1, session=active_session,
                address="bbbbbbbbbbbb", mtu=247))["result"]["generation"]
            pending_write = relay_client.event("ble_write", generation)
            allocated_nodes = catalog_records(credentials)
            assert len(allocated_nodes) == len(previous_nodes) + 1
            allocated_id, allocated_phase = allocated_nodes[-1]
            assert allocated_id > previous_nodes[-1][0] and allocated_phase == 1
            assert status(ipc_path, 9, 315, b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')['ok']
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 316)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(.01)
            relay.terminate()
            relay.wait(timeout=2)
            stdout, stderr = service.communicate(timeout=5)
            assert service.returncode != 0 and not stdout, (service.returncode, stdout, stderr)
            assert not stderr, stderr
            # Low-level OpenThread failure may bypass Rust destructors; startup
            # must reclaim the stale socket before answering below.
            assert ipc_path.exists() and thread_dataset(credentials) == previous_dataset
            relay_client.close()
            radio.close()
            if radio_path.exists() and stat.S_ISSOCK(radio_path.lstat().st_mode):
                radio_path.unlink()

            radio = ChannelRcp("127.0.0.1", tcp_nodelay=True)
            relay = subprocess.Popen([str(ROOT / "build/openthread-unix/sim-relay-host"),
                str(radio_path), radio.address, str(radio.port), "90"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 3
            while not radio_path.exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Restart relay did not start")
                time.sleep(.02)
            service = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5
            while True:
                if service.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Service did not wait for a fresh coordinator observation")
                try:
                    status(ipc_path, 1, 317)
                    break
                except (OSError, RuntimeError):
                    time.sleep(.02)
            assert status(ipc_path, 2, 318)["state"] == "awaiting_coordinator"
            assert not radio.requests
            stale = status(ipc_path, 8, 319, json.dumps(dict(schema=1, session=active_session,
                generation=generation, write=pending_write["write"], success=True)).encode())
            assert stale["error"] == "stale_session"
            wrong = status(ipc_path, 9, 320, b'{"schema":1,"action":"observe","zigbee_channel":11,"zigbee_online":true}')
            assert wrong["error"] == "not_ready" and not radio.requests
            assert status(ipc_path, 2, 321)["state"] == "channel_mismatch"
            assert status(ipc_path, 9, 322, b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')['ok']
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 323)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(.01)
            assert status(ipc_path, 2, 324)["configured"]
            recovered_nodes = catalog_records(credentials)
            assert recovered_nodes[:-1] == previous_nodes
            assert recovered_nodes[-1] == (allocated_id, 5)
            assert thread_dataset(credentials) == previous_dataset
            channel_writes = len(radio.channel_writes)
            gated_credentials = credentials.read_bytes()
            for ident, channel in ((325, 20), (327, 11)):
                mismatch = status(ipc_path, 9, ident, json.dumps(dict(schema=1, action="observe",
                    zigbee_channel=channel, zigbee_online=True)).encode())
                assert mismatch["error"] == "not_ready"
                assert status(ipc_path, 2, ident + 1)["state"] == "channel_mismatch"
                assert credentials.read_bytes() == gated_credentials
                assert len(radio.channel_writes) == channel_writes
            assert status(ipc_path, 9, 329, b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')['ok']
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 330)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(.01)
            assert len(radio.channel_writes) >= channel_writes
            assert radio.channel_writes and all(channel == 15 for channel in radio.channel_writes)

            # Status traffic stays responsive across multiple independent clients.
            start = time.monotonic()
            for ident in range(400, 420):
                assert status(ipc_path, ident % 2 + 1, ident)["available"]
            assert time.monotonic() - start < 2
            committed = credentials.read_bytes()
            metadata = credentials.stat()
            channel_writes_before_expiry = len(radio.channel_writes)
            time.sleep(5.1)
            expired = status(ipc_path, 2, 499)
            assert expired["state"] == "awaiting_coordinator" and not expired["attached"]
            assert "zigbee_channel" not in expired and expired["dataset_channel"] == 15
            assert credentials.read_bytes() == committed
            assert len(radio.channel_writes) == channel_writes_before_expiry

            service.terminate()
            stdout, stderr = service.communicate(timeout=2)
            assert service.returncode == 0
            assert not ipc_path.exists()
            assert not stdout
            assert not stderr, stderr
            assert not radio.error
            assert radio.counts.get("command_1", 0) == 0
            assert radio.channel_writes and all(channel == 15 for channel in radio.channel_writes)
            relay.wait(timeout=2)
            radio.close()
            original_image = state_image(private / "state")
            archive_path = private / "matter-state.tar"
            archive_state(private / "state", archive_path)
            preflight = subprocess.run(["python3", str(ARCHIVE_HELPER), str(archive_path)],
                capture_output=True, timeout=5)
            assert preflight.returncode == 0, preflight.stderr.decode(errors="replace")
            staged = private / "archive-staged-state"
            extracted = subprocess.run(["python3", str(ARCHIVE_HELPER), str(archive_path),
                "--extract-to", str(staged)], capture_output=True, timeout=5)
            assert extracted.returncode == 0, extracted.stderr.decode(errors="replace")
            staged_image = state_image(staged)
            checked = subprocess.run([str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter"),
                "--validate-state-dir", str(staged)], capture_output=True, timeout=5)
            assert checked.returncode == 0, checked.stderr.decode(errors="replace")
            assert checked.stderr == b"Matter state validated\n"
            assert state_image(staged) == staged_image
            assert state_image(private / "state") == original_image

            # Structurally valid archive content still has to pass Rust identity
            # and Matter-record checks after extraction.
            corrupt_source = private / "corrupt-source"
            shutil.copytree(private / "state", corrupt_source)
            corrupt_credentials = bytearray((corrupt_source / "credentials.bin").read_bytes())
            corrupt_credentials[-1] ^= 1
            (corrupt_source / "credentials.bin").write_bytes(corrupt_credentials)
            corrupt_archive = private / "corrupt-state.tar"
            archive_state(corrupt_source, corrupt_archive)
            corrupt_preflight = subprocess.run(["python3", str(ARCHIVE_HELPER), str(corrupt_archive)],
                capture_output=True, timeout=5)
            assert corrupt_preflight.returncode == 0
            corrupt_staged = private / "corrupt-staged-state"
            extracted = subprocess.run(["python3", str(ARCHIVE_HELPER), str(corrupt_archive),
                "--extract-to", str(corrupt_staged)], capture_output=True, timeout=5)
            assert extracted.returncode == 0
            corrupt_image = state_image(corrupt_staged)
            rejected = subprocess.run([str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter"),
                "--validate-state-dir", str(corrupt_staged)], capture_output=True, timeout=5)
            assert rejected.returncode == 1 and not rejected.stdout
            assert rejected.stderr == b"Matter state validation failed (private store)\n"
            assert state_image(corrupt_staged) == corrupt_image

            checked = subprocess.run([str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter"),
                "--validate-state-dir", str(private / "state")], capture_output=True, timeout=5)
            assert checked.returncode == 0, checked.stderr.decode(errors="replace")
            assert not checked.stdout and checked.stderr == b"Matter state validated\n"
            assert state_image(private / "state") == original_image
            incomplete = private / "incomplete-state"
            shutil.copytree(private / "state", incomplete)
            (incomplete / "credentials.bin").unlink()
            incomplete_image = state_image(incomplete)
            rejected = subprocess.run([str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter"),
                "--validate-state-dir", str(incomplete)], capture_output=True, timeout=5)
            assert rejected.returncode == 1 and not rejected.stdout
            assert rejected.stderr == b"Matter state validation failed (private store)\n"
            assert state_image(incomplete) == incomplete_image
            unsafe = private / "unsafe-state"
            shutil.copytree(private / "state", unsafe)
            os.chmod(unsafe / "tmp", 0o755)
            unsafe_image = state_image(unsafe)
            rejected = subprocess.run([str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter"),
                "--validate-state-dir", str(unsafe)], capture_output=True, timeout=5)
            assert rejected.returncode == 1 and not rejected.stdout
            assert rejected.stderr == b"Matter state validation failed (Thread settings)\n"
            assert state_image(unsafe) == unsafe_image
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
            assert not stderr, stderr
            wrong_vendor = subprocess.run(args + ["--administrator-vendor-id", "65522"], capture_output=True, timeout=2)
            assert wrong_vendor.returncode == 1
            assert not wrong_vendor.stderr
            assert credentials.read_bytes() == committed
            assert not ipc_path.exists()
            # Simulate a corrupt committed envelope only in isolated temporary state.
            credentials.write_bytes(committed[:8])
            corrupt = subprocess.run(args, capture_output=True, timeout=2)
            assert corrupt.returncode == 1
            assert not corrupt.stderr
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
    failed_apply_retry_gate()


def failed_apply_retry_gate():
    radio = ChannelRcp("127.0.0.1", tcp_nodelay=True)
    relay = service = None
    try:
        with tempfile.TemporaryDirectory(prefix="ot-sim-apply-retry-") as directory:
            private = Path(directory)
            radio_path = private / "radio.sock"
            ipc_path = private / "matter.sock"
            binary = str(ROOT / "build/micro-thread-matter-daemon/target/release/mthread-matter")
            state_path = private / "state"

            def start_relay(path, rcp):
                process = subprocess.Popen([str(ROOT / "build/openthread-unix/sim-relay-host"),
                    str(path), rcp.address, str(rcp.port), "30"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                deadline = time.monotonic() + 3
                while not path.exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Apply-retry relay did not start")
                    time.sleep(0.02)
                return process

            def start_service(path, socket_path):
                nonlocal service
                args = [binary, "--socket", str(socket_path), "--state-dir", str(state_path),
                    "--radio-url", f"spinel+hdlc+unix://{path}?no-reset"]
                service = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                deadline = time.monotonic() + 5
                while True:
                    if service.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Apply-retry service did not become responsive")
                    try:
                        assert status(socket_path, 1, 801)["state"] == "awaiting_coordinator"
                        return
                    except (OSError, RuntimeError):
                        time.sleep(0.02)

            relay = start_relay(radio_path, radio)
            start_service(radio_path, ipc_path)
            assert status(ipc_path, 9, 802,
                b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')['ok']
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 803)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(0.01)
            credentials = private / "state/credentials.bin"
            before = credentials.read_bytes()
            radio.fail_next_dataset_write = True
            try:
                status(ipc_path, 9, 804,
                    b'{"schema":1,"action":"create","zigbee_channel":15}')
            except Exception as error:
                assert isinstance(error, RuntimeError), error
            else:
                raise AssertionError("Spinel dataset rejection unexpectedly returned an IPC reply")
            assert radio.dataset_write_errors == 1, radio.dataset_write_errors
            failed_code = service.wait(timeout=5)
            _, failed_stderr = service.communicate(timeout=2)
            assert failed_code == 1, (failed_code, failed_stderr)
            assert not failed_stderr, failed_stderr
            failed_envelope = credentials.read_bytes()
            dataset = thread_dataset(credentials)
            assert dataset and failed_envelope != before

            # A genuine Spinel status error leaves the pinned OpenThread
            # instance unusable during cleanup, so the runtime exits and the
            # paired supervisor's restart path owns recovery. The committed
            # dataset must survive and restore on a fresh RCP connection.
            relay.terminate()
            relay.wait(timeout=3)
            radio.close()
            radio = ChannelRcp("127.0.0.1", tcp_nodelay=True)
            radio_path.unlink(missing_ok=True)
            ipc_path.unlink(missing_ok=True)
            relay = start_relay(radio_path, radio)
            start_service(radio_path, ipc_path)
            assert credentials.read_bytes() == failed_envelope
            assert status(ipc_path, 9, 805,
                b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}')['ok']
            deadline = time.monotonic() + 3
            while status(ipc_path, 1, 806)["state"] != "running":
                assert time.monotonic() < deadline
                time.sleep(0.01)
            restored = status(ipc_path, 2, 807)
            assert restored["configured"] and restored["dataset_channel"] == 15, restored
            assert radio.dataset_write_errors == 0 and radio.dataset_write_count > 0
            assert all(channel == 15 for channel in radio.channel_writes), radio.channel_writes
            assert not radio.error and radio.counts.get("command_1", 0) == 0
            assert credentials.read_bytes() == failed_envelope
            service.terminate()
            stdout, stderr = service.communicate(timeout=3)
            assert service.returncode == 0 and not stdout
            assert not stderr, stderr
            relay.wait(timeout=3)
            print("PASS Spinel apply rejection preserves the dataset and recovers after service restart")
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
