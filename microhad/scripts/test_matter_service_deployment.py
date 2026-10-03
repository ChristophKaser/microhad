#!/usr/bin/env python3
"""Exercise optional service staging with a fake SSH transport; no network access."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE_SSH = '''#!/usr/bin/env python3
import json, os, sys
data = sys.stdin.buffer.read()
with open(os.environ['STAGING_CALLS'], 'a') as output:
    output.write(json.dumps({'command':sys.argv[-1], 'bytes':len(data)})+'\\n')
'''


class Staging(unittest.TestCase):
    def test_service_stages_without_start_or_state_upload(self):
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
            subprocess.run(args, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           check=True, timeout=10)
            rows = [json.loads(line) for line in calls.read_text().splitlines()]
            uploads = [row for row in rows if row['bytes']]
            self.assertEqual(len(uploads), 3)
            self.assertEqual(uploads[1]['bytes'], service.stat().st_size)
            self.assertEqual(uploads[2]['bytes'], (ROOT / 'paired_start.sh').stat().st_size)
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


if __name__ == '__main__':
    unittest.main()
