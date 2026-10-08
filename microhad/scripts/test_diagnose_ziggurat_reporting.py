#!/usr/bin/env python3
"""Reporting diagnostic decoding and safety guards; no hardware access."""
import contextlib
import io
import struct
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import diagnose_ziggurat_reporting as diagnostic


class ReportingDiagnosticTests(unittest.TestCase):
    def frame(self, data):
        payload = struct.pack('<HHBHHHBBBBH', 0xa8d1, 0, 0, 0, 0x104,
                              0x402, 1, 1, 212, 186, len(data)) + data
        return b'\x10\x03\x00' + payload

    def test_real_temperature_report_and_truncation(self):
        frame = self.frame(bytes.fromhex('18000a000029b508'))
        row = diagnostic.aps(frame)
        self.assertEqual((row['source'], row['cluster'], row['command'], row['transaction']),
                         (0xa8d1, 0x402, 10, 0))
        self.assertIsNone(diagnostic.aps(frame[:-1]))
        self.assertIsNone(diagnostic.aps(b'\x11' + frame[1:]))

    def test_manufacturer_header_and_read_response_distinction(self):
        row = diagnostic.aps(self.frame(bytes.fromhex('1c3412e909000000')))
        self.assertEqual((row['command'], row['transaction']), (9, 0xe9))
        row = diagnostic.aps(self.frame(bytes.fromhex('180d0100000029c408')))
        self.assertEqual(row['command'], 1)

    def test_readback_and_alert_require_execute_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'out.jsonl'
            for mode in ['readback', 'alert']:
                argv = ['diagnostic', '--mode', mode, '--host', '127.0.0.1',
                        '--password-file', '/unused', '--output', str(destination)]
                with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        diagnostic.main()
                self.assertFalse(destination.exists())

    def test_observation_counts_reports_not_reads_or_other_devices(self):
        args = SimpleNamespace(since=100, duration=1, ieee='0x00124b002a52140a')
        rows = []
        status = {'online': True, 'network_formed': True, 'devices': [
            {'ieee_address': args.ieee, 'last_seen': 101,
             'temperature_celsius': 22.29, 'humidity_percent': 72.55}]}
        logs = {'lines': [
            '100 ' + args.ieee + ' command=0x1(Read Attributes Response)',
            '101 ' + args.ieee + ' command=0xa(Report Attributes)',
            '101 0x00124b002a5226de command=0xa(Report Attributes)']}
        with patch.object(diagnostic, 'http', side_effect=[status, logs]), \
             patch.object(diagnostic.time, 'time', return_value=101), \
             contextlib.redirect_stdout(io.StringIO()):
            diagnostic.observe(args, rows.append)
        self.assertEqual(rows[-1]['reports'], 1)
        self.assertEqual(rows[-1]['offline_samples'], 0)

    def test_wire_ieee_uses_device_byte_order(self):
        self.assertEqual(diagnostic.wire_ieee(SimpleNamespace(ieee='0x00124b002a52140a')),
                         bytes.fromhex('0a14522a004b1200'))


if __name__ == '__main__':
    unittest.main()
