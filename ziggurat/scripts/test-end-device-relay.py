#!/usr/bin/env python3
"""Verify actual-server forwarding with synthetic keys and a loopback radio."""
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

spec = importlib.util.spec_from_file_location("sim", Path(__file__).with_name("simulate-rcp.py"))
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


class CaptureRadio(sim.Rcp):
    def __init__(self):
        self.transmissions = queue.Queue()
        super().__init__("127.0.0.1", iid=1)

    def receive_frame(self, data):
        command, offset = sim.unpacked(data, 1)
        if command == 3:
            prop, offset = sim.unpacked(data, offset)
            if prop == 113:
                length = struct.unpack_from("<H", data, offset)[0]
                # Spinel TX includes the two-byte MAC FCS; it is outside CCM.
                self.transmissions.put(data[offset + 2:offset + length])
        super().receive_frame(data)


def main():
    radio = CaptureRadio()
    client = process = None
    with tempfile.TemporaryDirectory(prefix="ziggurat-relay-") as directory, tempfile.TemporaryFile() as log:
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
            # Broadcast retries wait for router passive acknowledgements. Keep
            # one synthetic router in the audience, as on the real network.
            state["children"].append({"ieee": "0200000000000022", "nwk": 0x2222,
                                      "type": 2, "rx_on_when_idle": True})
            for command, payload in import_commands(state):
                client.request(command, payload)
            client.request(0x16)
            child = bytes.fromhex(state["children"][0]["ieee"])
            aes = AESCCM(bytes(range(16)), tag_length=4)

            # Give the router a live outgoing link cost. Imported neighbors
            # alone are not expected relayers until they exchange link status.
            router = bytes.fromhex("0200000000000022")
            nwk = struct.pack("<HHHBB", 0x1209, 0xfffc, 0x2222, 1, 1) + router
            aux = b"\x28" + struct.pack("<I", 1000) + router + b"\x07"
            ciphertext = aes.encrypt(router + aux[1:5] + b"\x2d",
                                     b"\x08\x61\x00\x00\x11", nwk + b"\x2d" + aux[1:])
            radio.inject(struct.pack("<HBHHH", 0x8841, 1, 0x1a62, 0xffff, 0x2222)
                         + nwk + aux + ciphertext)
            # A report after the link status provides a processing fence.
            nwk = struct.pack("<HHHBB", 0x0208, 0, 0x2222, 12, 2)
            aux = b"\x28" + struct.pack("<I", 1001) + router + b"\x07"
            report = bytes.fromhex("000102040401010118010a000029b508")
            ciphertext = aes.encrypt(router + aux[1:5] + b"\x2d", report,
                                     nwk + b"\x2d" + aux[1:])
            radio.inject(struct.pack("<HBHHH", 0x8841, 2, 0x1a62, 0, 0x2222)
                         + nwk + aux + ciphertext)
            deadline = time.monotonic() + 4
            while True:
                event = client.receive(timeout=max(0.01, deadline - time.monotonic()))
                if event[0] == 0x10 and event[1] & 3 == 3:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("missing topology processing fence")

            # A sleepy child's group toggle is MAC-unicast to its parent, but
            # NWK-broadcast to all receiver-on devices, as in the live failure.
            for sequence, initiator in ((56, True), (57, False)):
                control = 0x0248 | (0x2000 if initiator else 0)
                nwk = struct.pack("<HHHBB", control, 0xfffd, 0x1234, 12, sequence)
                aux = b"\x28" + struct.pack("<I", 1000 + sequence) + child + b"\x07"
                authenticated = b"\x2d" + aux[1:]
                aps = b"\x0c\x03\x00\x06\x00\x04\x01\x01" + bytes([sequence]) + b"\x01\x17\x02"
                ciphertext = aes.encrypt(child + aux[1:5] + b"\x2d", aps, nwk + authenticated)
                mac = struct.pack("<HBHHH", 0x8861, sequence, 0x1a62, 0, 0x1234)
                radio.inject(mac + nwk + aux + ciphertext)

                deadline = time.monotonic() + 4
                copies = 0
                while copies < 3:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("missing broadcast relay/retry")
                    frame = radio.transmissions.get(timeout=remaining)
                    if len(frame) < 31 or frame[13:15] != b"\x34\x12" or frame[16] != sequence:
                        continue
                    forwarded = struct.unpack_from("<H", frame, 9)[0]
                    assert not forwarded & 0x2000, "relay retained End Device Initiator"
                    assert forwarded == control & ~0x2000, "unrelated control bits changed"
                    assert frame[5:9] == b"\xff\xff\x00\x00", "wrong MAC relay addresses"
                    assert frame[11:15] == b"\xfd\xff\x34\x12", "NWK addresses changed"
                    assert frame[15] == 11, "radius not decremented exactly once"
                    authenticated = b"\x2d" + frame[18:31]
                    nonce = frame[22:30] + frame[18:22] + b"\x2d"
                    plaintext = aes.decrypt(nonce, frame[31:], frame[9:17] + authenticated)
                    assert plaintext == aps, "group command changed during forwarding"
                    copies += 1
            print("PASS: group relay and all retries clear initiator, preserve addresses/payload, and authenticate")
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
                if process.returncode not in (0, -15):
                    log.seek(0)
                    print(log.read().decode(errors="replace"), file=sys.stderr)
            radio.close()


if __name__ == "__main__":
    main()
