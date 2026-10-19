#!/usr/bin/env python3
"""Host-only checks for gateway API log capture."""
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch
from urllib.error import URLError

spec = importlib.util.spec_from_file_location('capture_thread_logs',
                                            Path(__file__).with_name('capture-thread-logs.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureTests(unittest.TestCase):
    def test_inclusive_boundary_preserves_repeated_messages(self):
        cursor = capture.LogCursor()
        self.assertEqual(cursor.consume(['10 first', '11 repeated', '11 repeated']),
                         ['10 first', '11 repeated', '11 repeated'])
        self.assertEqual(cursor.consume(['11 repeated', '11 repeated', '11 repeated', '11 new']),
                         ['11 repeated', '11 new'])
        self.assertEqual(cursor.consume(['11 repeated', '11 repeated', '11 repeated', '11 new']), [])
        self.assertEqual(cursor.consume([]), [])
        self.assertEqual(cursor.consume(['11 repeated', '11 repeated', '11 repeated', '11 new']), [])
        self.assertEqual(cursor.consume(['11 new', '12 next']), ['12 next'])
        self.assertEqual(cursor.since, 12)

    def test_out_of_order_timestamps_use_highest_boundary(self):
        cursor = capture.LogCursor()
        self.assertEqual(cursor.consume(['12 newer', '11 delayed']), ['12 newer', '11 delayed'])
        self.assertEqual(cursor.since, 12)
        self.assertEqual(cursor.consume(['12 newer', '12 added']), ['12 added'])

    def test_poll_retries_and_keeps_cursor_after_request_failure(self):
        output = io.StringIO()
        responses = [io.StringIO('{"lines":["10 first"]}'), URLError('unavailable'),
                     io.StringIO('{"lines":["10 first","10 second","11 next"]}')]
        with patch.object(capture, 'urlopen', side_effect=responses) as request, \
             patch.object(capture.time, 'sleep', side_effect=[None, None, KeyboardInterrupt]), \
             patch.object(capture.sys, 'stderr', io.StringIO()) as errors:
            with self.assertRaises(KeyboardInterrupt):
                capture.capture('gateway.example', output)
        self.assertEqual(output.getvalue(), '10 first\n10 second\n11 next\n')
        self.assertEqual([call.args[0] for call in request.call_args_list],
                         ['http://gateway.example/api/logs?since=0',
                          'http://gateway.example/api/logs?since=10',
                          'http://gateway.example/api/logs?since=10'])
        self.assertIn('retrying', errors.getvalue())


if __name__ == '__main__':
    unittest.main()
