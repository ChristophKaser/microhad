#!/usr/bin/env python3
"""Exercise optional service staging with a fake SSH transport; no network access."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE_SSH = '''#!/usr/bin/env python3
import json, os, sys
data = sys.stdin.buffer.read()
command = sys.argv[-1]
with open(os.environ['STAGING_CALLS'], 'a') as output:
    output.write(json.dumps({'command':command, 'bytes':len(data)})+'\\n')
if 'df -k /tuya' in command:
    print(os.environ.get('FLASH_AVAILABLE_KIB', '1000000'))
elif 'for image in' in command:
    print(os.environ.get('REMOTE_RUNNING_IMAGES', '0:0\\n0:0'))
elif 'for file in' in command and 'wc -c' in command:
    print(os.environ.get('REMOTE_SIZES', '0 0 0 0 0'))
'''


def matter_manifest(binary, matter=1):
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    return (f'format=microhad-build-v1\nziggurat=1\nmatter={matter}\n'
            f'binary_sha256={digest}\n')


class Staging(unittest.TestCase):
    def test_paired_launcher_enables_matter_only_for_staged_executable(self):
        for staged in (False, True):
            with self.subTest(staged=staged), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                native = root / 'native';native.mkdir()
                if staged:
                    service = native / 'thread-matter';service.write_text('#!/bin/sh\nexit 0\n')
                    service.chmod(0o700)
                capture = root / 'matter-bin'
                paired = native / 'paired_start.sh'
                paired.write_text('#!/bin/sh\nprintf "%s" "${MATTER_BIN-}" > "$MATTER_CAPTURE"\n')
                paired.chmod(0o700)
                set_time = root / 'set_time.sh';set_time.write_text('#!/bin/sh\nexit 0\n')
                set_time.chmod(0o700)
                launcher_text = (ROOT / 'ziggurat_start.sh').read_text()
                launcher_text = launcher_text.replace('/tuya/native', str(native))
                launcher_text = launcher_text.replace('/tuya/set_time.sh', str(set_time))
                launcher = root / 'ziggurat_start.sh';launcher.write_text(launcher_text)
                env = dict(os.environ, MATTER_CAPTURE=str(capture))
                env.pop('MATTER_BIN', None)
                subprocess.run(['sh', str(launcher)], env=env, check=True, timeout=2)
                for _ in range(50):
                    if capture.exists():
                        break
                    time.sleep(0.01)
                self.assertTrue(capture.exists())
                expected = str(native / 'thread-matter') if staged else ''
                self.assertEqual(capture.read_text(), expected)

    def test_service_stages_without_start_or_state_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'fake native');binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            service = root / 'thread-matter';service.write_bytes(b'fake service');service.chmod(0o700)
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ,PATH=str(root)+os.pathsep+os.environ['PATH'],STAGING_CALLS=str(calls))
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--matter-service', str(service),
                    '--target', '/tuya/native-test']
            subprocess.run(args, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           check=True, timeout=10)
            rows = [json.loads(line) for line in calls.read_text().splitlines()]
            uploads = [row for row in rows if row['bytes']]
            self.assertEqual(len(uploads), 5)
            self.assertEqual(uploads[1]['bytes'], len(Path(str(binary) + '.matter-manifest').read_bytes()))
            self.assertEqual(uploads[2]['bytes'], service.stat().st_size)
            self.assertEqual(uploads[3]['bytes'], (ROOT / 'paired_start.sh').stat().st_size)
            self.assertEqual(uploads[4]['bytes'], (ROOT / 'scripts/paired_control.sh').stat().st_size)
            commands = '\n'.join(row['command'] for row in rows)
            self.assertIn('/tuya/native-test/.thread-matter.new', commands)
            self.assertIn('chmod 700', commands)
            for forbidden in ('killall', '/tuya/tuya_start.sh', 'creds.bin', '/dev/ttyS1'):
                self.assertNotIn(forbidden, commands)
            calls.unlink()
            result = subprocess.run(args + ['--webui-only'], env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(calls.exists())

    def test_supervisor_stages_without_reuploading_matter_service(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'fake native');binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ, PATH=str(root)+os.pathsep+os.environ['PATH'], STAGING_CALLS=str(calls))
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--supervisor', '--binary', str(binary), '--target', '/tuya/native']
            result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=True, timeout=10)
            self.assertIn('Matter flash preflight passed', result.stdout)
            rows = [json.loads(line) for line in calls.read_text().splitlines()]
            uploads = [row for row in rows if row['bytes']]
            self.assertEqual(len(uploads), 4)
            self.assertTrue(any('/.paired_start.sh.new' in row['command'] for row in uploads))
            self.assertTrue(any('/.paired_control.sh.new' in row['command'] for row in uploads))
            self.assertTrue(any('/.microhad.matter-manifest.new' in row['command'] for row in uploads))
            self.assertFalse(any('/.thread-matter.new' in row['command'] for row in rows))
            calls.unlink()
            env['FLASH_AVAILABLE_KIB'] = '1'
            result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Insufficient /tuya flash', result.stderr)
            self.assertFalse(any(row['bytes'] for row in map(json.loads, calls.read_text().splitlines())))

    def test_matter_flash_preflight_refuses_before_any_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'fake native');binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            service = root / 'thread-matter';service.write_bytes(b'fake service');service.chmod(0o700)
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ, PATH=str(root)+os.pathsep+os.environ['PATH'],
                       STAGING_CALLS=str(calls), FLASH_AVAILABLE_KIB='1',
                       REMOTE_SIZES='0 0 0 0 0')
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--matter-service', str(service),
                    '--target', '/tuya/native-test']
            result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Insufficient /tuya flash for Matter staging', result.stderr)
            rows = [json.loads(line) for line in calls.read_text().splitlines()]
            self.assertEqual(len(rows), 3)
            self.assertIn('for file in', rows[0]['command'])
            self.assertIn('for image in', rows[1]['command'])
            self.assertIn('df -k /tuya', rows[2]['command'])
            self.assertTrue(all(row['bytes'] == 0 for row in rows))

    def test_matter_flash_preflight_counts_replaced_in_place_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'n' * 1741444);binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            service = root / 'thread-matter';service.write_bytes(b's' * 2158276);service.chmod(0o700)
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ, PATH=str(root)+os.pathsep+os.environ['PATH'],
                       STAGING_CALLS=str(calls), FLASH_AVAILABLE_KIB='3532',
                       REMOTE_SIZES='1504596 0 0 0 7417')
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--matter-service', str(service),
                    '--target', '/tuya/native']
            result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Matter flash preflight passed', result.stdout)

    def test_matter_test_target_is_refused_at_observed_flash_headroom(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'n' * 1741444);binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            service = root / 'thread-matter';service.write_bytes(b's' * 2158276);service.chmod(0o700)
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ, PATH=str(root)+os.pathsep+os.environ['PATH'],
                       STAGING_CALLS=str(calls), FLASH_AVAILABLE_KIB='3532',
                       REMOTE_SIZES='0 0 0 0 0')
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--matter-service', str(service),
                    '--target', '/tuya/native-test']
            result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Insufficient /tuya flash for Matter staging', result.stderr)
            rows = [json.loads(line) for line in calls.read_text().splitlines()]
            self.assertEqual(len(rows), 3)
            self.assertTrue(all(row['bytes'] == 0 for row in rows))

    def test_explicit_paired_activation_installs_matter_aware_launcher(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'fake native');binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            service = root / 'thread-matter';service.write_bytes(b'fake service');service.chmod(0o700)
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ,PATH=str(root)+os.pathsep+os.environ['PATH'],STAGING_CALLS=str(calls))
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--matter-service', str(service),
                    '--target', '/tuya/native', '--activate-paired']
            result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = [json.loads(line) for line in calls.read_text().splitlines()]
            uploads = [row for row in rows if row['bytes']]
            self.assertEqual([row['bytes'] for row in uploads], [binary.stat().st_size,
                len(Path(str(binary) + '.matter-manifest').read_bytes()), service.stat().st_size,
                (ROOT / 'paired_start.sh').stat().st_size,
                (ROOT / 'scripts/paired_control.sh').stat().st_size,
                (ROOT / 'ziggurat_start.sh').stat().st_size])
            commands = '\n'.join(row['command'] for row in rows)
            self.assertIn('cp /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup', commands)
            self.assertIn('mv "/tuya/native/.gateway_start.sh.new" /tuya/tuya_start.sh', commands)
            self.assertIn('export MATTER_BIN=/tuya/native/thread-matter',
                          (ROOT / 'ziggurat_start.sh').read_text())
            self.assertNotIn('killall', commands)
            self.assertIn('Existing processes were not restarted', result.stdout)

    def test_paired_activation_requires_explicit_service_and_native_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'fake native');binary.chmod(0o700)
            Path(str(binary) + '.matter-manifest').write_text(matter_manifest(binary))
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ,PATH=str(root)+os.pathsep+os.environ['PATH'],STAGING_CALLS=str(calls))
            base = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--activate-paired']
            result = subprocess.run(base, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(calls.exists())
            service = root / 'thread-matter';service.write_bytes(b'fake service');service.chmod(0o700)
            result = subprocess.run(base + ['--matter-service', str(service), '--target', '/tuya/native-test'],
                                    env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(calls.exists())
            staging = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                       '--binary-only', '--matter-service', str(service), '--target', '/tuya/native-test']
            result = subprocess.run(staging, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                    text=True, check=False, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('ZIGGURAT=1 MATTER=1', result.stderr)
            self.assertFalse(calls.exists())

    def test_matter_deployment_rejects_disabled_or_stale_build_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / 'sshpass';fake.write_text(FAKE_SSH);fake.chmod(0o700)
            binary = root / 'native';binary.write_bytes(b'fake native');binary.chmod(0o700)
            service = root / 'thread-matter';service.write_bytes(b'fake service');service.chmod(0o700)
            password = root / 'password';password.touch()
            calls = root / 'calls'
            env = dict(os.environ,PATH=str(root)+os.pathsep+os.environ['PATH'],STAGING_CALLS=str(calls))
            args = ['sh', str(ROOT / 'deploy.sh'), '--password-file', str(password),
                    '--binary-only', '--binary', str(binary), '--matter-service', str(service),
                    '--target', '/tuya/native-test']
            manifest = Path(str(binary) + '.matter-manifest')
            for body in (matter_manifest(binary, matter=0), matter_manifest(binary).replace(
                    hashlib.sha256(binary.read_bytes()).hexdigest(), '0' * 64)):
                manifest.write_text(body)
                result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                        text=True, check=False, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(calls.exists())


if __name__ == '__main__':
    unittest.main()
