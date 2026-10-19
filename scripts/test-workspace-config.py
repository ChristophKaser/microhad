#!/usr/bin/env python3
"""Host-only configuration and SSH transport checks. Never contacts a gateway."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

from workspace_config import ROOT, settings, ssh_command

KEYS = 'GATEWAY_IP LAN_INTERFACE TOOLCHAIN LINK_MODE RSDK_ROOT CROSSTOOL_NG_ROOT STOCK_ROOTFS SSH_PASSWORD_FILE SSH_PASSWORD SSH_USER SSH_PORT SSH_CONNECT_TIMEOUT'.split()


class WorkspaceSettings(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='workspace-settings-')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.config = self.directory / 'local settings.env'
        self.config.write_text('')
        self.env = {k: v for k, v in os.environ.items()
                    if k not in KEYS and not k.startswith('TARGET_')}
        self.env['WORKSPACE_CONFIG'] = str(self.config)

    def run_loader(self, *args, env=None, cwd='/tmp'):
        return subprocess.run(['sh', str(ROOT / 'scripts/workspace-env.sh'), *args],
                              env=env or self.env, cwd=cwd, capture_output=True, check=True)

    def resolved(self):
        with patch.dict(os.environ, self.env, clear=True):
            return settings()

    def test_source_quotes_unknown_keys_and_relative_paths(self):
        self.config.write_text("""# ordinary shell configuration
export GATEWAY_IP=192.0.2.20 # comment
LAN_INTERFACE='lan test'
RSDK_ROOT='build/compiler space'
SSH_PASSWORD_FILE='../password file'
EXTRA_SETTING=ignored
SSH_PASSWORD='fixture password $literal # quote'
""")
        values = self.resolved()
        self.assertEqual(values['GATEWAY_IP'], '192.0.2.20')
        self.assertEqual(values['LAN_INTERFACE'], 'lan test')
        self.assertEqual(values['RSDK_ROOT'], str(ROOT / 'build/compiler space'))
        self.assertEqual(values['SSH_PASSWORD_FILE'], str(ROOT.parent / 'password file'))
        self.assertNotIn('EXTRA_SETTING', values)
        self.assertNotIn('SSH_PASSWORD', values)
        self.assertEqual(values['SSH_PASSWORD_SET'], '1')
        self.assertNotIn(b'fixture password', self.run_loader('--null').stdout)

    def test_environment_wins_and_is_quoted_as_data(self):
        self.config.write_text('GATEWAY_IP=192.0.2.20\nLAN_INTERFACE=from-file\n')
        self.env.update(GATEWAY_IP='192.0.2.30', LAN_INTERFACE="a'b; $(touch /tmp/workspace-must-not-execute)")
        values = self.resolved()
        self.assertEqual(values['GATEWAY_IP'], self.env['GATEWAY_IP'])
        self.assertEqual(values['LAN_INTERFACE'], self.env['LAN_INTERFACE'])
        self.env['GATEWAY_IP'] = ''
        self.assertEqual(self.resolved()['GATEWAY_IP'], '')

    def test_missing_local_file_and_example(self):
        # A copied loader supplies a disposable workspace with no local env file.
        root = self.directory / 'clean-workspace'
        (root / 'scripts').mkdir(parents=True)
        (root / 'config').mkdir()
        shutil.copy(ROOT / 'scripts/workspace-env.sh', root / 'scripts/workspace-env.sh')
        shutil.copy(ROOT / 'config/toolchains.mk', root / 'config/toolchains.mk')
        env = dict(self.env)
        del env['WORKSPACE_CONFIG']
        result = subprocess.run(['sh', str(root / 'scripts/workspace-env.sh'), '--get', 'TOOLCHAIN'],
                                env=env, check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), 'rsdk')
        self.env['WORKSPACE_CONFIG'] = str(ROOT / 'workspace.local.env.example')
        self.assertEqual(self.resolved()['GATEWAY_IP'], '')
        self.assertEqual(self.resolved()['SSH_PASSWORD_SET'], '')

    def test_invalid_config_reports_no_secret(self):
        for text in ["SSH_PASSWORD='fixture-secret", 'TOOLCHAIN=invalid', 'SSH_PORT=0', 'SSH_PORT=65536', 'SSH_CONNECT_TIMEOUT=no']:
            self.config.write_text(text + '\n')
            result = subprocess.run(['sh', str(ROOT / 'scripts/workspace-env.sh'), '--null'],
                                    env=self.env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b'fixture-secret', result.stdout + result.stderr)
        self.env['WORKSPACE_CONFIG'] = str(self.directory / 'nonexistent')
        with self.assertRaises(ValueError):
            self.resolved()

    def test_make_shell_python_compiler_agreement_and_overrides(self):
        for backend in ['rsdk', 'crosstool-ng']:
            self.config.write_text(f'TOOLCHAIN={backend}\nRSDK_ROOT="build/rsdk space"\nCROSSTOOL_NG_ROOT=build/candidate\n')
            env = dict(self.env)
            python = subprocess.check_output(['python3', str(ROOT / 'scripts/toolchain_config.py'), 'TARGET_CC'], env=env, text=True).strip()
            shell = subprocess.check_output(['bash', '-c', 'source "$1/scripts/toolchains.sh"; printf "%s" "$TARGET_CC"', 'sh', str(ROOT)], env=env, text=True)
            recipe = f'ZIGGURAT_ROOT := {ROOT}\ninclude {ROOT}/config/toolchains.mk\ninclude {ROOT}/config/toolchain-select.mk\n$(info COMPILER=$(TARGET_CC))\nall:;@:\n'
            result = subprocess.run(['make', '--no-print-directory', '-f', '-'], input=recipe,
                                    env=env, cwd='/tmp', capture_output=True, text=True, check=True)
            make = next(row.removeprefix('COMPILER=') for row in result.stdout.splitlines() if row.startswith('COMPILER='))
            self.assertEqual(python, shell)
            self.assertEqual(python, make)
            result = subprocess.run(['make', '--no-print-directory', '-f', '-', 'TOOLCHAIN=rsdk', 'TARGET_CC=/explicit/compiler'], input=recipe,
                                    env=dict(env, TOOLCHAIN='crosstool-ng'), cwd='/tmp', capture_output=True, text=True, check=True)
            self.assertIn('COMPILER=/explicit/compiler', result.stdout)
            env.update(TOOLCHAIN=backend, RSDK_ROOT='build/environment-rsdk', CROSSTOOL_NG_ROOT='build/environment-candidate')
            python = subprocess.check_output(['python3', str(ROOT / 'scripts/toolchain_config.py'), 'TARGET_CC'], env=env, cwd='/tmp', text=True).strip()
            result = subprocess.run(['make', '--no-print-directory', '-f', '-'], input=recipe,
                                    env=env, cwd='/tmp', capture_output=True, text=True, check=True)
            self.assertIn('COMPILER=' + python, result.stdout)

    def fake_sshpass(self):
        executable = self.directory / 'sshpass'
        executable.write_text('''#!/usr/bin/env python3
import json, os, pathlib, stat, sys, time
password = pathlib.Path(sys.argv[2])
assert '-f' == sys.argv[1]
assert 'SSH_PASSWORD' not in os.environ
with open(os.environ['SSH_RECORD'], 'w') as output:
    json.dump({'args': sys.argv[3:], 'credential_path': str(password),
               'mode': stat.S_IMODE(password.stat().st_mode),
               'password': password.read_text(), 'stdin': sys.stdin.buffer.read().hex()}, output)
if os.environ.get('SSH_WAIT'):
    time.sleep(30)
sys.exit(int(os.environ.get('SSH_EXIT', '0')))
''')
        executable.chmod(0o700)
        env = dict(self.env, PATH=str(self.directory) + os.pathsep + self.env['PATH'],
                   SSH_RECORD=str(self.directory / 'record.json'))
        return env

    def test_inline_password_transport_preserves_stdin_exit_status_and_cleanup(self):
        secret = "fixture ' password $() #"
        self.config.write_text('SSH_PASSWORD=' + shlex.quote(secret) + '\nSSH_USER=operator\nSSH_PORT=2222\nSSH_CONNECT_TIMEOUT=12\n')
        env = self.fake_sshpass()
        env['SSH_EXIT'] = '7'
        command = ssh_command('192.0.2.20') + ['cat']
        result = subprocess.run(command, env=env, input=b'\x00upload\xff', capture_output=True)
        self.assertEqual(result.returncode, 7)
        record = json.loads(Path(env['SSH_RECORD']).read_text())
        self.assertEqual(record['password'], secret + '\n')
        self.assertEqual(record['mode'], 0o600)
        self.assertEqual(record['stdin'], b'\x00upload\xff'.hex())
        self.assertIn('operator', record['args'])
        self.assertIn('2222', record['args'])
        self.assertIn('ConnectTimeout=12', record['args'])
        self.assertNotIn(secret, str(command) + str(record['args']) + str(result.stdout + result.stderr))
        self.assertFalse(Path(record['credential_path']).exists())

    def test_explicit_password_file_overrides_and_conflicts_fail(self):
        password = self.directory / 'password file'
        password.write_text('explicit\n')
        self.config.write_text('SSH_PASSWORD=inline\nSSH_PASSWORD_FILE=/missing\n')
        env = self.fake_sshpass()
        command = ssh_command('192.0.2.20', password) + ['true']
        result = subprocess.run(command, env=env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(Path(env['SSH_RECORD']).read_text())['password'], 'explicit\n')
        self.assertTrue(password.exists())
        result = subprocess.run(ssh_command('192.0.2.20') + ['true'], env=env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'not both', result.stderr)

    def test_inline_password_cleanup_on_termination(self):
        self.config.write_text('SSH_PASSWORD=fixture\n')
        env = dict(self.fake_sshpass(), SSH_WAIT='1')
        process = subprocess.Popen(ssh_command('192.0.2.20') + ['true'], env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.addCleanup(lambda: process.poll() is not None or os.killpg(process.pid, signal.SIGKILL))
        deadline = time.monotonic() + 5
        while not Path(env['SSH_RECORD']).exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        record = json.loads(Path(env['SSH_RECORD']).read_text())
        process.terminate()
        process.communicate(timeout=5)
        self.assertEqual(process.returncode, 143)
        self.assertFalse(Path(record['credential_path']).exists())

    def test_device_mode_stays_explicit(self):
        self.config.write_text('GATEWAY_IP=192.0.2.20\nSSH_PASSWORD=fixture\n')
        name = ROOT / 'scripts/matter-diagnostics/commission.py'
        spec = importlib.util.spec_from_file_location('commission_config_test', name)
        module = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, self.env, clear=True), patch('sys.path', [str(name.parent)] + __import__('sys').path):
            spec.loader.exec_module(module)
            args = module.parser().parse_args(['--payload', 'MT:fixture'])
            self.assertEqual(args.gateway, '192.0.2.20')
        # Capture parser results and stop before any fixture builds or I/O.
        import argparse
        class Parsed(Exception):
            pass
        original = argparse.ArgumentParser.parse_args
        for relative in ['microhad/scripts/test-matter-ipc-codec.py', 'micro-thread-matter-daemon/scripts/replay-thread-fragment.py']:
            name = ROOT / relative
            spec = importlib.util.spec_from_file_location('device_gate_test', name)
            module = importlib.util.module_from_spec(spec)
            with patch.dict(os.environ, self.env, clear=True), patch('sys.path', [str(name.parent)] + __import__('sys').path):
                spec.loader.exec_module(module)
                for flags, expected in [([], None), (['--host' if 'codec' in relative else '--device'], '192.0.2.20')]:
                    def parse(parser, *args, **kwargs):
                        argv = ([] if 'codec' in relative else ['unused']) + flags
                        result = original(parser, argv)
                        self.assertEqual(getattr(result, 'host' if 'codec' in relative else 'device'), expected)
                        raise Parsed()
                    with patch.object(argparse.ArgumentParser, 'parse_args', parse), self.assertRaises(Parsed):
                        module.main()

    def test_otbr_receives_explicit_host_settings(self):
        # Fake processes prove expansion and argument forwarding without Docker,
        # network access, or modifying Thread state.
        script = ROOT / 'scripts/matter-diagnostics/otbr-entrypoint.sh'
        self.assertNotIn('192.168.178.62', script.read_text())
        result = subprocess.run(['bash', str(script)], env=dict(self.env, GATEWAY='', BACKBONE_INTERFACE=''), capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'GATEWAY', result.stderr)


if __name__ == '__main__':
    unittest.main()
