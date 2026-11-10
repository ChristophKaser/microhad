#!/usr/bin/env python3
"""Check deployment argument resolution without executing deployment or SSH."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ArtifactPaths(unittest.TestCase):
    def resolve(self, *args, mode='dynamic'):
        # Execute the actual initialization/parser, stopping before validation
        # or any transport access. Keep $0 pointed at the real script root.
        script = (ROOT / 'deploy.sh').read_text().split('if [ -n "$service_selection" ]; then', 1)[0]
        script += '\nprintf "%s\\n" "$binary" "$matter_service" "$ziggurat" "$maintenance_helper" "$ftp_collector" "$webui_dir"\n'
        env = dict(os.environ, WORKSPACE_CONFIG=str(ROOT / 'workspace.local.env.example'), LINK_MODE=mode)
        return subprocess.run(['sh', '-c', script, str(ROOT / 'deploy.sh'), *args],
                              cwd=ROOT, env=env, text=True, capture_output=True, timeout=10)

    def test_defaults_follow_link_mode_for_every_service(self):
        for mode, subdir, spec in [('static', 'crosstool-ng/', 'lexra-crosstool-ng-uclibc'),
                                   ('dynamic', 'crosstool-ng/dynamic/', 'lexra-crosstool-ng-uclibc-dynamic')]:
            with self.subTest(mode=mode):
                result = self.resolve('--maintenance-deploy', mode=mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), [
                    str(ROOT / f'microhad/build/{subdir}mips-ziggurat-matter/microhad'),
                    str(ROOT / f'build/{subdir}micro-thread-matter-daemon/mthread-matter-mips'),
                    str(ROOT / f'build/{subdir}ziggurat-hdlc/{spec}/release/ziggurat'),
                    str(ROOT / f'build/{subdir}gateway-maintenance'),
                    str(ROOT / f'build/{subdir}ftp-log-collector'),
                    str(ROOT / 'microhad/webui/dist')])
                self.assertNotIn('WARNING', result.stderr)

    def test_service_selection_and_activation_need_no_paths(self):
        for args in [('--services', 'native,matter,ziggurat'), ('--activate-paired',),
                     ('--activate-paired-only',), ('--binary-only', '--matter')]:
            with self.subTest(args=args):
                result = self.resolve(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.splitlines()[1].endswith('/mthread-matter-mips'))
        result = self.resolve('--binary-only')
        self.assertEqual(result.stdout.splitlines()[1:4], ['', '', ''])

    def test_overrides_warn_and_equivalent_default_paths_do_not(self):
        flags = ['--binary', '--matter-service', '--ziggurat', '--maintenance-helper', '--ftp-collector', '--webui-dir']
        defaults = self.resolve('--maintenance-deploy').stdout.splitlines()
        for flag, default in zip(flags, defaults):
            with self.subTest(flag=flag):
                result = self.resolve(flag, '/tmp/other-artifact')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f'WARNING: {flag} selects a non-default artifact: /tmp/other-artifact', result.stderr)
                self.assertIn(f'Configured build output: {default}', result.stderr)
                result = self.resolve(flag, os.path.relpath(default, ROOT))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn('WARNING', result.stderr)


if __name__ == '__main__':
    unittest.main()
