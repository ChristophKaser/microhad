#!/usr/bin/env python3
"""Host-only syslog integration using synthetic service traffic and libc interception."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / 'build/micro-thread-matter-daemon/target/release/mthread-matter'


def main():
    with tempfile.TemporaryDirectory(prefix='matter-logging-') as directory:
        private = Path(directory)
        capture = private / 'syslog.txt'
        shim = private / 'syslog-capture.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'micro-thread-matter-daemon/tests/helpers/syslog-capture.c'),
                        '-o', str(shim)], check=True)
        env = dict(os.environ, LD_PRELOAD=str(shim), MATTER_SYSLOG_CAPTURE=str(capture))
        for level in ('off', 'error', 'warn', 'info', 'debug', 'trace'):
            capture.unlink(missing_ok=True)
            result = subprocess.run([str(BINARY), '--log-level', level,
                                     '--state-dir', 'relative-path'], env=env, capture_output=True)
            assert result.returncode != 0
            records = capture.read_text() if capture.exists() else ''
            assert ('ERROR mthread_matter: Matter controller service failed' in records) == (level != 'off')
            assert not result.stderr
        for invalid in ('verbose', 'INFO', ''):
            capture.unlink(missing_ok=True)
            result = subprocess.run([str(BINARY), '--log-level', invalid], env=env, capture_output=True)
            assert result.returncode != 0 and not capture.exists()
            assert b'Matter controller service failed' in result.stderr
        capture.unlink(missing_ok=True)
        env['MICROHAD_MATTER_TEST_LOG_LEVEL'] = 'trace'
        subprocess.run(['python3', str(ROOT / 'micro-thread-matter-daemon/scripts/test-matter-service.py')],
                       env=env, check=True)
        records = capture.read_text().splitlines()
        assert any('Matter controller service started' in record for record in records)
        assert any('rs_matter' in record for record in records), 'library logging is not connected'
        assert any('TRACE ' in record for record in records), 'trace logging is not connected'
        for record in records:
            tag, severity, body = record.split('\t', 2)
            assert tag == 'mthread-matter'
            assert int(severity) in (3, 4, 6, 7)
            assert len(body.encode()) <= 174
            assert len(body.encode()) + 64 + len(tag) + 1 < 255
            assert '\x1b' not in body
        print(f'PASS Matter CLI filtering, daemon/library trace logging, and {len(records)} bounded syslog records')


if __name__ == '__main__':
    main()
