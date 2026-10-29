#!/usr/bin/env python3
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('receiver', HERE / 'receiver.py')
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)
BINARY = HERE.parents[1] / 'build/gateway-core-host'


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='core-test-', dir='/tmp')
        self.root = Path(self.temp.name)
        self.output = self.root / 'received'
        self.output.mkdir()
        self.server = receiver.Receiver(('127.0.0.1', 0), receiver.Capture)
        self.server.gateway = '127.0.0.1'
        self.server.output = self.output
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.config = self.root / 'config'
        self.config.write_text(f'127.0.0.1 {self.server.server_address[1]}\n')
        self.spool = self.root / 'spool'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def capture(self, data=b'ELF-test', retry=False, snapshot=False):
        args = ['--retry'] if retry else [str(os.getpid()), '11', '123']
        if snapshot:
            args.insert(0, '--snapshot')
        return subprocess.run([str(BINARY), '--paths', str(self.config), str(self.spool), *args],
                              input=data, capture_output=True, timeout=25)

    def metadata(self):
        return [json.loads(p.read_text()) for p in self.output.glob('*/capture.json')]

    def offline(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.config.write_text(f'127.0.0.1 {sock.getsockname()[1]}\n')

    def online(self):
        self.config.write_text(f'127.0.0.1 {self.server.server_address[1]}\n')

    def test_killed_transport_prefix_recovery(self):
        import struct
        self.spool.mkdir(mode=0o700)
        wire = b'GCORE1 1 11 123\n' + struct.pack('!I', 4) + b'meta'
        wire += struct.pack('!I', 6) + b'prefix' + struct.pack('!I', 100) + b'incomplete'
        (self.spool/'partial').write_bytes(wire)
        self.assertEqual(self.capture(retry=True).returncode, 0)
        self.assertEqual(next(self.output.glob('*/core.truncated')).read_bytes(), b'prefix')
        self.assertFalse((self.spool/'partial').exists())

    def test_snapshot_length_validation(self):
        import struct
        data = b'ELF snapshot'
        self.assertEqual(self.capture(struct.pack('!Q', len(data))+data, snapshot=True).returncode, 0)
        self.assertTrue(self.metadata()[0]['complete'])
        self.assertEqual(self.capture(struct.pack('!Q', len(data)+100)+data, snapshot=True).returncode, 0)
        self.assertEqual(len(list(self.output.glob('*/core.truncated'))), 1)

    def test_successful_new_stream_preserves_older_fallback(self):
        self.offline(); self.capture(b'older')
        pending = self.spool/'pending'; original = pending.read_bytes()
        self.online(); self.assertEqual(self.capture(b'newer').returncode, 0)
        self.assertEqual(pending.read_bytes(), original)

    def test_large_unlimited_stream(self):
        data = os.urandom(5 * 1024 * 1024 + 7)
        self.assertEqual(self.capture(data).returncode, 0)
        self.assertEqual(next(self.output.glob('*/core')).read_bytes(), data)
        self.assertFalse((self.spool / 'pending').exists())
        self.assertTrue(self.metadata()[0]['complete'])

    def test_offline_bounded_prefix_and_retry(self):
        self.offline()
        data = os.urandom(3 * 1024 * 1024)
        self.assertEqual(self.capture(data).returncode, 1)
        pending = self.spool / 'pending'
        self.assertLessEqual(pending.stat().st_size, 1024 * 1024)
        original = pending.read_bytes()
        self.assertEqual(self.capture(b'new crash').returncode, 1)
        self.assertEqual(pending.read_bytes(), original)
        self.online()
        self.assertEqual(self.capture(retry=True).returncode, 0)
        recovered = next(self.output.glob('*/core.truncated')).read_bytes()
        self.assertTrue(data.startswith(recovered))
        self.assertGreater(len(recovered), 900000)
        self.assertFalse(pending.exists())
        self.assertFalse(self.metadata()[0]['complete'])

    def test_small_fallback_is_complete(self):
        self.offline()
        self.assertEqual(self.capture(b'small').returncode, 1)
        self.online()
        self.assertEqual(self.capture(retry=True).returncode, 0)
        self.assertEqual(next(self.output.glob('*/core')).read_bytes(), b'small')

    def test_concurrent_capture_skipped(self):
        self.spool.mkdir(mode=0o700)
        with (self.spool / 'lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.assertEqual(self.capture().returncode, 1)
        self.assertFalse(list(self.output.iterdir()))

    def test_storage_failure_no_ack(self):
        with patch.object(receiver.os, 'fsync', side_effect=OSError('disk full')):
            self.assertEqual(self.capture().returncode, 1)
        self.assertTrue((self.spool / 'pending').exists())
        self.assertFalse(list(self.output.glob('*/core')))

    def test_interrupted_transfer(self):
        with socket.create_connection(self.server.server_address) as sock:
            sock.sendall(b'GCORE1 1 11 123\n\x00\x00\x00\x04meta\x00\x00\x00\x04ab')
        # Wait for the server to finish recording its incomplete result.
        import time
        for _ in range(100):
            if self.metadata():
                break
            time.sleep(.01)
        self.assertFalse(self.metadata()[0]['complete'])
        self.assertFalse(list(self.output.glob('*/core')))

    def test_midstream_disconnect_and_stall(self):
        for stall in (False, True):
            with self.subTest(stall=stall):
                listener = socket.socket()
                listener.bind(('127.0.0.1', 0)); listener.listen()
                self.config.write_text(f'127.0.0.1 {listener.getsockname()[1]}\n')
                def consume():
                    import time
                    conn, _ = listener.accept()
                    with conn:
                        conn.recv(128)
                        if stall:
                            time.sleep(12)
                thread = threading.Thread(target=consume)
                thread.start()
                self.assertEqual(self.capture(os.urandom(6 * 1024 * 1024)).returncode, 1)
                thread.join(); listener.close()
                self.assertLessEqual((self.spool / 'pending').stat().st_size, 1024 * 1024)
                (self.spool / 'pending').unlink()


if __name__ == '__main__':
    unittest.main()
