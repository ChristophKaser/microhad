#!/usr/bin/env python3
"""Actual-server replay regression using synthetic keys and a loopback RCP only."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'microhad/scripts'))
from test_network_migration import fixture
from ziggurat_migration import import_commands
from ziggurat_cobs import Client
spec = importlib.util.spec_from_file_location('sim', ROOT / 'ziggurat/scripts/simulate-rcp.py')
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def main():
    radio = sim.Rcp('127.0.0.1', iid=1)
    process = client = None
    with tempfile.TemporaryDirectory(prefix='ziggurat-join-replay-') as directory, tempfile.TemporaryFile() as log:
        try:
            path = Path(directory) / 'api.sock'
            process = subprocess.Popen([str(ROOT / 'build/host/debug/ziggurat'), '--device',
                'tcp://127.0.0.1:' + str(radio.port), '--spinel-iid', '1', '--api', 'cobs-unix',
                '--listen', 'unix:' + str(path), '--log-level', 'debug'], stdout=log, stderr=log)
            deadline = time.monotonic() + 8
            while not path.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('host server startup failed')
                time.sleep(0.02)
            client = Client.connect(path)
            state = fixture()
            state['network']['tclk_seed'] = None
            for command, payload in import_commands(state):
                client.request(command, payload)
            client.request(0x16)
            child = bytes.fromhex(state['children'][0]['ieee'])
            router = bytes.fromhex('0200000000000022')
            sequence = 0
            router_counter = 1000
            fence_marker = 180

            def encrypted(sender, source, counter, aps, key_sequence=7):
                nonlocal sequence
                sequence += 1
                nwk = struct.pack('<HHHBB', 0x0208, 0, source, 30, sequence)
                aux = b'\x28' + struct.pack('<I', counter) + sender + bytes([key_sequence])
                authenticated_aux = b'\x2d' + aux[1:]
                nonce = sender + aux[1:5] + b'\x2d'
                ciphertext = AESCCM(bytes(range(16)), tag_length=4).encrypt(nonce, aps, nwk + authenticated_aux)
                mac = struct.pack('<HBHHH', 0x8841, sequence, 0x1a62, 0, source)
                return mac + nwk + aux + ciphertext

            def report(marker):
                return b'\x00\x01' + struct.pack('<HH', 0x402, 0x104) + bytes([1, marker]) + bytes.fromhex('18') + bytes([marker]) + bytes.fromhex('0a000029b508')

            def receive_until_fence(marker, joined=False):
                found = False
                got_join = False
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    frame = client.receive(timeout=4)
                    if frame[0] == 0x20 and frame[1] & 3 == 3 and frame[5:13] == child:
                        got_join = True
                    if frame[0] == 0x10 and frame[1] & 3 == 3:
                        value = frame[3:]
                        data = value[17:]
                        if len(data) >= 3 and data[1] == marker:
                            found = True
                        if len(data) >= 3 and data[1] == fence_marker:
                            return got_join if joined else found
                raise TimeoutError('processing fence not received')

            def fence():
                nonlocal router_counter, fence_marker
                router_counter += 1
                fence_marker += 1
                radio.inject(encrypted(router, 0x2222, router_counter, report(fence_marker)))

            def inject_report(counter, marker, sender=None, source=0x1234, key_sequence=7):
                radio.inject(encrypted(child if sender is None else sender, source, counter, report(marker), key_sequence))
                fence()
                return receive_until_fence(marker)

            def update(status):
                nonlocal router_counter
                router_counter += 1
                aps = bytes([1, sequence + 1, 6]) + child + struct.pack('<H', 0x1234) + bytes([status])
                radio.inject(encrypted(router, 0x2222, router_counter, aps))
                fence()
                return receive_until_fence(249, joined=True)

            assert inject_report(100, 1), 'initial authenticated report missing'
            assert not inject_report(1, 2), 'stale report accepted before join'
            assert update(0), 'secured rejoin not processed'
            assert not inject_report(2, 3), 'secured rejoin cleared replay protection'
            assert update(3), 'trust-center rejoin not processed'
            assert not inject_report(2, 10), 'trust-center rejoin cleared replay protection'
            assert not update(1), 'fresh join accepted outside permit window'
            assert not inject_report(3, 4), 'denied join cleared replay protection'
            client.request(0x34, struct.pack('<HB', 60, 1))
            before = client.request(0x20)
            assert update(1), 'authorized fresh join not processed'
            invalid = bytearray(encrypted(child, 0x1234, 500, report(11)))
            invalid[-1] ^= 1
            radio.inject(bytes(invalid)); fence()
            assert not receive_until_fence(11), 'invalid MIC report accepted'
            assert inject_report(4, 5), 'fresh join retained old NWK counter: new reports rejected'
            assert not inject_report(4, 6), 'duplicate accepted after new baseline'
            assert inject_report(5, 7), 'next fresh report rejected'
            assert not inject_report(1, 8, sender=router, source=0x2222), 'other relayer baseline cleared'
            assert not inject_report(6, 9, key_sequence=6), 'wrong network key sequence accepted'
            after = client.request(0x20)
            assert after[22:39] == before[22:39], 'network key identity changed'
            assert int.from_bytes(after[39:43], 'little') >= int.from_bytes(before[39:43], 'little'), 'outgoing counter regressed'
            print('PASS actual encrypted reports recover only after an authorized fresh join')
            print('PASS ordinary secured rejoins, denied joins, duplicates and other relayers retain replay protection')
            print('PASS network identity and outgoing counter retained; synthetic host radio only')
        except Exception:
            log.seek(0)
            # Synthetic test state only, never a production key/log.
            print(log.read().decode(errors='replace')[-4000:])
            raise
        finally:
            if client:
                client.close()
            if process:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            radio.close()
    if radio.error:
        raise RuntimeError('host radio simulator failed')


if __name__ == '__main__':
    main()
