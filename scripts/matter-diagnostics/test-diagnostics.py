#!/usr/bin/env python3
"""Host-only capture, exporter verification and process-cleanup regressions."""
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout

from cryptography.hazmat.primitives.ciphers.aead import AESCCM
import commission
import spinel_capture

spec = importlib.util.spec_from_file_location('verify_export', Path(__file__).with_name('verify-export.py'))
export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export)


def session_directory(root):
    for name in ('capture', 'keys', 'metadata'):
        (root / name).mkdir(exist_ok=True)


def encrypted_fixture(root):
    session_directory(root)
    keys, metadata, messages = [], [], []
    for context, kind, resumed in ((1, 'PASE', False), (2, 'CASE', False), (3, 'CASE', True)):
        key = bytes([context]) * 16
        keys.append({'event': 'keys', 'pid': 1, 'context': context, 'tx_key': key.hex(),
                     'rx_key': key.hex(), 'resumed': resumed})
        metadata.append({'event': 'session', 'pid': 1, 'context': context, 'type': kind})
        for index, direction in enumerate(('tx', 'rx')):
            nonce, aad, plaintext = bytes([context, index]) + bytes(11), b'header', b'fixture plaintext'
            cipher = AESCCM(key, tag_length=16).encrypt(nonce, plaintext, aad)
            messages.append({'event': 'message', 'pid': 1, 'context': context, 'direction': direction,
                             'nonce': nonce.hex(), 'aad': aad.hex(), 'ciphertext': cipher[:-16].hex(),
                             'tag': cipher[-16:].hex(), 'plaintext': plaintext.hex() if direction == 'tx' else None})
    (root / 'keys/matter-session-keys.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in keys + metadata))
    (root / 'capture/matter-messages.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in messages))


def frame(payload):
    crc = 0xffff
    for byte in payload:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    result = bytearray(b'\x7e')
    for byte in payload + struct.pack('<H', crc ^ 0xffff):
        if byte in (0x7d, 0x7e, 0x11, 0x13, 0xf8):
            result.extend((0x7d, byte ^ 0x20))
        else:
            result.append(byte)
    return bytes(result) + b'\x7e'


def capture(path, gap=False, reset=False, previous=False):
    result = bytearray(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 276))
    def packet(reverse, seq, payload=b'', flags=0x18, ephemeral=23456):
        ip = bytearray(20)
        ip[0], ip[9] = 0x45, 6
        ip[12:16], ip[16:20] = (b'\xc0\xa8\xb2\x3e', b'\xc0\xa8\xb2\x33') if reverse else \
            (b'\xc0\xa8\xb2\x33', b'\xc0\xa8\xb2\x3e')
        struct.pack_into('>H', ip, 2, 40 + len(payload))
        tcp = bytearray(20)
        struct.pack_into('>HHI', tcp, 0, *((9998, ephemeral, seq) if reverse else (ephemeral, 9998, seq)))
        tcp[12], tcp[13] = 0x50, flags
        body = b'\x08\x00' + bytes(18) + ip + tcp + payload
        result.extend(struct.pack('<IIII', 0, 0, len(body), len(body)) + body)
    if previous:
        packet(False, 9000, frame(b'\x81\x03\x71\x00\x00'), ephemeral=12345)
        packet(True, 9100, frame(b'\x80\x06\x71\x00\x00'), ephemeral=12345)
    packet(False, 100, flags=2)
    packet(True, 300, flags=0x12)
    payload = frame(b'\x81\x03\x71\x00\x00')
    packet(False, 101, payload)
    packet(False, 101, payload) # Retransmission must not produce duplicate Spinel frames.
    packet(True, 301, frame(b'\x80\x06\x71\x00\x00'))
    if gap:
        packet(False, 102 + len(payload), payload)
    if reset:
        packet(False, 101 + len(payload), frame(b'\x81\x01'))
    path.write_bytes(result)


class DecryptionTests(unittest.TestCase):
    def test_pase_case_and_resumption_both_directions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            encrypted_fixture(root)
            result = export.verify(root)
            self.assertTrue(result['complete'])
            self.assertEqual(len(result['authenticated_messages']), 6)

    def test_corrupted_tag_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            encrypted_fixture(root)
            path = root / 'capture/matter-messages.jsonl'
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[0]['tag'] = '00' * 16
            path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
            self.assertFalse(export.verify(root)['complete'])


class CaptureTests(unittest.TestCase):
    def test_fresh_handshake_and_raw_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'fixture.pcap'
            capture(path)
            result = spinel_capture.inspect(path, '192.168.178.62', 9998)
            self.assertTrue(result['complete'])
            self.assertEqual(result['frames'], 2)

    def test_gap_and_reset_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'fixture.pcap'
            for mode in ('gap', 'reset'):
                capture(path, **{mode: True})
                self.assertFalse(spinel_capture.inspect(path, '192.168.178.62', 9998)['complete'])

    def test_incomplete_pcap_record_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'fixture.pcap'
            capture(path)
            with path.open('ab') as output:
                output.write(b'\x00' * 8)
            self.assertFalse(spinel_capture.inspect(path, '192.168.178.62', 9998)['complete'])

    def test_existing_connection_does_not_mask_fresh_handshake(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'fixture.pcap'
            capture(path, previous=True)
            result = spinel_capture.inspect(path, '192.168.178.62', 9998)
            self.assertTrue(result['complete'])
            self.assertEqual(result['ignored_midstream_directions'], 2)


class LoggerSplitTests(unittest.TestCase):
    def test_all_attribute_bodies_retained_and_repeat_is_idempotent(self):
        source = commission.ROOT / 'build/connectedhomeip/source'
        original = source / 'zzz_generated/chip-tool/zap-generated/cluster/logging/DataModelLogger.cpp'
        if not original.exists():
            self.skipTest('Pinned SDK checkout not available')
        spec = importlib.util.spec_from_file_location('split_logger', Path(__file__).with_name('split-logger.py'))
        splitter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(splitter)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / original.relative_to(source)
            target.parent.mkdir(parents=True)
            target.write_text(original.read_text())
            (root / 'examples/chip-tool/commands/clusters').mkdir(parents=True)
            result = splitter.split(root)
            self.assertEqual(result['attribute_clusters'], 158)
            self.assertLess(result['largest_unit_lines'], 2000)
            split = target.parent / 'split'
            joined = ''.join(p.read_text() for p in sorted(split.glob('*.cpp')))
            import re
            attribute_cases = re.findall(r'^        case (.+::Attributes::.+::Id):', original.read_text(), re.M)
            self.assertTrue(attribute_cases)
            for case in attribute_cases:
                self.assertEqual(joined.count('case ' + case + ':'), 1, case)
            before = {p: p.stat().st_mtime_ns for p in split.iterdir()}
            splitter.split(root)
            self.assertEqual(before, {p: p.stat().st_mtime_ns for p in split.iterdir()})


class WorkflowTests(unittest.TestCase):
    def attempt(self, temporary):
        args = commission.parser().parse_args(['--payload', 'MT:TEST', '--output-root', temporary, '--tail-seconds', '0'])
        result = commission.Attempt(args)
        result.sudo = []
        return result

    def run_workflow(self, code=0, interrupted=False, failed_collector=False):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            attempt.dataset = '00'
            attempt.preflight = lambda: None
            attempt.start_collectors = lambda: None
            attempt.start_otbr_log = lambda: None
            attempt.docker = lambda *a, **k: None
            attempt.cli = lambda *a: ''
            attempt.wait_ready = lambda: None
            attempt.snapshot = lambda stage: ('00', {})
            if interrupted:
                attempt.chip = lambda *a: (_ for _ in ()).throw(KeyboardInterrupt())
            else:
                attempt.chip = lambda *a: code
            if failed_collector:
                attempt.start_collectors = lambda: (_ for _ in ()).throw(RuntimeError('capture failed'))
            encrypted_fixture(attempt.directory)
            with patch.object(signal, 'signal'):
                status = attempt.run()
            result = json.loads((attempt.directory / 'result.json').read_text())
            self.assertEqual(result['success'], code == 0 and not interrupted and not failed_collector)
            self.assertEqual(status == 0, result['success'])
            self.assertTrue((attempt.directory / 'metadata/sha256.json').exists())
            return result

    def test_success(self):
        self.assertEqual(self.run_workflow()['read_exit'], 0)

    def test_commission_failure_skips_read(self):
        self.assertIsNone(self.run_workflow(code=7)['read_exit'])

    def test_timeout_retained(self):
        self.assertEqual(self.run_workflow(code=124)['commission_exit'], 124)

    def test_interrupt_preserves_bundle(self):
        self.assertIn('Interrupted', self.run_workflow(interrupted=True)['errors'])

    def test_collector_failure_prevents_commission(self):
        self.assertIsNone(self.run_workflow(failed_collector=True)['commission_exit'])

    def test_cleanup_only_owned_collector(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            attempt.snapshot = lambda stage: ('00', {})
            other = subprocess.Popen(['sleep', '60'])
            try:
                attempt.collector('owned', ['sleep', '60'], required=False)
                process = attempt.collectors[0][1]
                for _ in range(100):
                    if attempt.collectors[0][2].read_text():
                        break
                    import time
                    time.sleep(.01)
                attempt.finish()
                self.assertIsNotNone(process.poll())
                self.assertIsNone(other.poll())
            finally:
                other.terminate()
                other.wait()

    def test_privileged_launcher_keeps_authenticated_terminal_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            attempt.sudo = ['sudo', '-n', '--']
            with patch.object(commission.subprocess, 'Popen') as popen:
                attempt.collector('privileged', ['tcpdump', '--version'], privileged=True)
            command = popen.call_args.args[0]
            self.assertEqual(command[:3], ['sudo', '-n', '--'])
            self.assertFalse(popen.call_args.kwargs.get('start_new_session', False))

    def test_collector_supervisor_retains_callers_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            attempt.snapshot = lambda stage: ('00', {})
            attempt.collector('session', ['sleep', '60'], required=False)
            process = attempt.collectors[0][1]
            try:
                self.assertEqual(os.getsid(process.pid), os.getsid(0))
            finally:
                attempt.finish()

    def test_gateway_capture_uses_log_api_collector(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            command = attempt.gateway_command()
            self.assertEqual(command[0], sys.executable)
            self.assertTrue(command[1].endswith('/scripts/capture-thread-logs.py'))
            self.assertEqual(command[2:], ['--host', attempt.args.gateway])

    def test_chip_output_is_mirrored_and_saved_including_final_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            controller = Path(temporary) / 'chip-tool'
            controller.write_text('#!/usr/bin/env python3\n'
                                  'import sys,time\n'
                                  'print("SDK output", flush=True)\n'
                                  'print("SDK error", file=sys.stderr, flush=True)\n'
                                  'time.sleep(.25)\n'
                                  'print("Final SDK output", flush=True)\n'
                                  'raise SystemExit(3)\n')
            controller.chmod(0o700)
            attempt.args.controller = controller
            attempt.save('metadata/controller-build.json', {'binary_sha256': 'test'})
            attempt.ensure_collectors = lambda: None
            console = io.StringIO()
            with redirect_stdout(console):
                code = attempt.chip('commission', ['pairing'], 3)
            self.assertEqual(code, 3)
            log = (attempt.directory / 'logs/commission.log').read_text()
            for text in ('SDK output', 'SDK error', 'Final SDK output'):
                self.assertIn(text, console.getvalue())
                self.assertIn(text, log)
            self.assertIn('exited with status 3', console.getvalue())

    def test_progress_is_flushed_to_console_and_saved(self):
        with tempfile.TemporaryDirectory() as temporary:
            attempt = self.attempt(temporary)
            with redirect_stdout(io.StringIO()) as console:
                attempt.progress('Starting capture')
            self.assertIn('Starting capture', console.getvalue())
            self.assertEqual(console.getvalue(), (attempt.directory / 'logs/runner.log').read_text())


if __name__ == '__main__':
    os.umask(0o077)
    unittest.main()
