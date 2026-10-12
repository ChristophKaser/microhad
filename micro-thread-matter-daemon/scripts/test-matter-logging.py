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
        subprocess.run(['bash', '-c', 'source scripts/rust-env.sh; '
                        'CARGO_TARGET_DIR=build/micro-thread-matter-daemon/target '
                        'cargo +"$RUST_TOOLCHAIN" build --offline --locked --release '
                        '--manifest-path micro-thread-matter-daemon/Cargo.toml '
                        '--features controller-service --example logging-capture-probe'],
                       cwd=ROOT, check=True)
        probe = ROOT / 'build/micro-thread-matter-daemon/target/release/examples/logging-capture-probe'
        for mode in ('off', 'on'):
            capture.unlink(missing_ok=True)
            subprocess.run([str(probe), mode], env=env, check=True)
            records = capture.read_text()
            assert 'capture probe ready' in records
            assert 'general library trace must stay disabled' not in records
            if mode == 'on':
                assert 'RX unix_us=' in records and 'peer=[fd00::1234]:5540' in records
                assert 'length=3 payload=[00, AB, FF]' in records
                assert 'length=2 result=0 payload=[01, CD]' in records
                assert 'length=1 result=4 payload=[02]' in records
            else:
                assert 'matter_raw' not in records
        print('PASS asynchronous UDP payload capture, peer metadata, send results, filtering and shutdown flush')
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
        result = subprocess.run([str(BINARY), '--raw-frames', '--log-level', 'info',
                                 '--state-dir', 'relative-path'], env=env, capture_output=True)
        assert result.returncode != 0 and not result.stderr
        assert 'ERROR mthread_matter: Matter controller service failed' in capture.read_text()
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
        capture.unlink(missing_ok=True)
        env['MICROHAD_MATTER_TEST_LOG_LEVEL'] = 'info'
        env['MICROHAD_MATTER_TEST_RAW_FRAMES'] = 'on'
        subprocess.run(['python3', str(ROOT / 'micro-thread-matter-daemon/scripts/test-matter-service.py')],
                       env=env, check=True)
        records = capture.read_text().splitlines()
        assert any('Matter controller service started' in record for record in records)
        assert not any('TRACE rs_matter' in record or 'DEBUG rs_matter' in record for record in records)
        print('PASS raw-frame mode preserves info logging and suppresses verbose library trace')


if __name__ == '__main__':
    main()
