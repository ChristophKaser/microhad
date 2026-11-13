#!/usr/bin/env python3
"""Check failed first-fragment diagnostics against a synthetic host RCP."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    # FRAG1: 80-byte datagram, tag 0x1234, invalid compressed payload 0xff.
    with tempfile.TemporaryDirectory(prefix='ot-fragment-diagnostics-') as directory:
        fragment = Path(directory) / 'invalid.hex'
        fragment.write_text('c0501234ff\n')
        result = subprocess.run(
            ['python3', str(ROOT / 'micro-thread-matter-daemon/scripts/replay-thread-fragment.py'), str(fragment)],
            cwd=ROOT, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    expected = (
        'RX fragment conversion failed stage:priority_headers error:NotFound dglen:80',
        'RX first fragment failed stage:frame_to_message error:NotFound tag:4660 dglen:80 msglen:0',
        'RX fragment input tag:4660 len:1 header:ff',
        'HandleFragment queued=0',
    )
    if result.returncode == 0 or any(message not in output for message in expected):
        raise AssertionError('Invalid fragment did not produce the expected rejection diagnostics:\n' + output)
    print('PASS invalid first-fragment rejection reports conversion stage and bounded input')


if __name__ == '__main__':
    main()
