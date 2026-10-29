#!/usr/bin/env python3
"""Exercise bounded neighbor bootstrap and pending-route recovery on a loopback RCP."""
import importlib.util
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import time

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "microhad/scripts"))
from test_network_migration import fixture
from ziggurat_migration import import_commands
from ziggurat_cobs import Client

spec = importlib.util.spec_from_file_location("relay", Path(__file__).with_name("test-end-device-relay.py"))
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)


def main():
    radio = relay.CaptureRadio()
    client = process = None
    aes = AESCCM(bytes(range(16)), tag_length=4)
    router = bytes.fromhex("0200000000000022")
    sequence = 0
    counter = 1000
    with tempfile.TemporaryDirectory(prefix="neighbor-recovery-") as directory, tempfile.TemporaryFile() as log:
        try:
            path = Path(directory) / "api.sock"
            process = subprocess.Popen([
                str(ROOT / "build/host/debug/ziggurat"), "--device",
                "tcp://127.0.0.1:" + str(radio.port), "--spinel-iid", "1",
                "--api", "cobs-unix", "--listen", "unix:" + str(path),
                "--log-level", "debug"], stdout=log, stderr=log)
            deadline = time.monotonic() + 8
            while not path.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("host server startup failed")
                time.sleep(0.02)
            client = Client.connect(path)
            state = fixture()
            state["network"]["tclk_seed"] = None
            for command, payload in import_commands(state):
                client.request(command, payload)
            client.request(0x16)

            def inject(command, valid=True):
                nonlocal sequence, counter
                sequence = (sequence + 1) % 256
                counter += 1
                nwk = struct.pack("<HHHBB", 0x1209, 0xfffc, 0x2222, 1, sequence) + router
                aux = b"\x28" + struct.pack("<I", counter) + router + b"\x07"
                encrypted = aes.encrypt(router + aux[1:5] + b"\x2d", command,
                                        nwk + b"\x2d" + aux[1:])
                if not valid:
                    encrypted = encrypted[:-1] + bytes([encrypted[-1] ^ 1])
                radio.inject(struct.pack("<HBHHH", 0x8841, sequence, 0x1a62, 0xffff, 0x2222)
                             + nwk + aux + encrypted)

            def decoded(frame):
                # These fixtures use short-address MAC frames and no source route.
                if len(frame) < 31 or frame[0:2] not in (b"\x41\x88", b"\x61\x88"):
                    return None
                control = struct.unpack_from("<H", frame, 9)[0]
                offset = 17 + (8 if control & 0x0800 else 0) + (8 if control & 0x1000 else 0)
                if not control & 0x0200 or control & 0x0400:
                    return None
                aux = frame[offset:offset + 14]
                if len(aux) != 14:
                    return None
                plain = aes.decrypt(aux[5:13] + aux[1:5] + b"\x2d", frame[offset + 14:],
                                    frame[9:offset] + b"\x2d" + aux[1:])
                return control, struct.unpack_from("<H", frame, 11)[0], plain, frame[15]

            def wait_for(predicate, timeout=4):
                end = time.monotonic() + timeout
                while time.monotonic() < end:
                    try:
                        frame = radio.transmissions.get(timeout=max(0.01, end - time.monotonic()))
                    except queue.Empty:
                        break
                    item = decoded(frame)
                    if item and predicate(item):
                        return item
                raise TimeoutError("missing expected synthetic radio transmission")

            # Drain boot's empty status; unauthenticated and incomplete lists must
            # not cause extra bootstrap transmissions.
            wait_for(lambda item: item[0] & 3 == 1 and item[2][:1] == b"\x08")
            inject(b"\x08\x60", valid=False)
            inject(b"\x08\x20")  # First fragment without last-fragment flag.
            start = time.monotonic()
            try:
                wait_for(lambda item: item[0] & 3 == 1 and item[2][:1] == b"\x08", 2.2)
                raise AssertionError("invalid/incomplete status triggered bootstrap")
            except TimeoutError:
                pass

            # Floods coalesce into one encrypted radius-one populated response.
            start = time.monotonic()
            for _ in range(8):
                inject(b"\x08\x60")
            status = wait_for(lambda item: item[0] & 3 == 1 and item[2][:1] == b"\x08", 3)
            elapsed = time.monotonic() - start
            assert 0.9 <= elapsed <= 2.8, elapsed
            assert status[2][1] & 0x1f, "bootstrap did not advertise the requesting router"
            assert status[1] == 0xfffc and status[3] == 1, "bootstrap must remain one-hop"
            try:
                wait_for(lambda item: item[0] & 3 == 1 and item[2][:1] == b"\x08", 1)
                raise AssertionError("empty-status flood caused multiple responses")
            except TimeoutError:
                pass

            def send(destination):
                payload = (b"\x00" + struct.pack("<H", destination) + bytes(8)
                           + struct.pack("<HHBBBBbBH", 0x104, 6, 1, 1, 0, 30, 0, 0, 3)
                           + b"\x11\x7a\x01")
                client.request(0x30, payload)
                return client.sequence

            def request_for(destination):
                return lambda item: (item[0] & 3 == 1 and item[2][:1] == b"\x01"
                                     and struct.unpack_from("<H", item[2], 3)[0] == destination)

            # Discovery starts with no bidirectional link. Restoring one triggers
            # a new request; reply to it and observe the queued APS payload.
            send(0x3333)
            initial = wait_for(request_for(0x3333))
            inject(b"\x08\x61\x00\x00\x11")
            retry = wait_for(lambda item: request_for(0x3333)(item)
                             and item[2][2] != initial[2][2])
            request_id = retry[2][2]
            # A route reply is a unicast command, rather than radius-one status.
            sequence = (sequence + 1) % 256
            counter += 1
            nwk = struct.pack("<HHHBB", 0x0209, 0, 0x2222, 30, sequence)
            aux = b"\x28" + struct.pack("<I", counter) + router + b"\x07"
            reply = b"\x02\x00" + bytes([request_id]) + struct.pack("<HHB", 0, 0x3333, 1)
            encrypted = aes.encrypt(router + aux[1:5] + b"\x2d", reply, nwk + b"\x2d" + aux[1:])
            radio.inject(struct.pack("<HBHHH", 0x8861, sequence, 0x1a62, 0, 0x2222)
                         + nwk + aux + encrypted)
            wait_for(lambda item: item[1] == 0x3333 and item[0] & 3 == 0
                     and item[2].endswith(b"\x11\x7a\x01"))

            # A second discovery with no reply expires on its original deadline,
            # despite two lost/recovered transitions. No failed command is replayed.
            start = time.monotonic()
            failed_id = send(0x4444)
            initial = wait_for(request_for(0x4444))
            # Recover halfway through discovery: extending its deadline would
            # now delay failure to 15 seconds and fail the 11.5-second assertion.
            time.sleep(5)
            for _ in range(2):
                inject(b"\x08\x60")
                inject(b"\x08\x61\x00\x00\x11")
                time.sleep(0.15)
            request_ids = {initial[2][2]}
            while time.monotonic() - start < 11.5:
                try:
                    item = wait_for(request_for(0x4444), 0.5)
                    request_ids.add(item[2][2])
                except TimeoutError:
                    pass
            assert len(request_ids) == 2, request_ids
            # Terminal confirmation is stronger evidence than diagnostic logging:
            # the caller must receive timeout within the original discovery window.
            confirmation = b"\x11" + struct.pack("<H", failed_id << 2 | 3) + b"\x01"
            deadline = time.monotonic() + 0.5
            events = list(client.events)
            while confirmation not in events and time.monotonic() < deadline:
                try:
                    events.append(client.receive(timeout=0.2))
                except TimeoutError:
                    pass
            assert confirmation in events, ("missing timely route timeout", events)
            # Recovery after failure must not resurrect the discarded command.
            inject(b"\x08\x60")
            inject(b"\x08\x61\x00\x00\x11")
            try:
                wait_for(request_for(0x4444), 0.5)
                raise AssertionError("failed command was rediscovered after recovery")
            except TimeoutError:
                pass
            print("PASS: jittered/coalesced bootstrap, authenticated recovery, queued delivery, bounded retry and expiry")
        except Exception:
            log.seek(0)
            print(log.read().decode(errors="replace"), file=sys.stderr)
            raise
        finally:
            if client:
                client.close()
            if process:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            radio.close()


if __name__ == "__main__":
    main()
