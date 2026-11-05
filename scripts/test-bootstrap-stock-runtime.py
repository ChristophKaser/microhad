#!/usr/bin/env python3
"""Exercise staged publication, hash validation and explicit local library access."""
import hashlib
import contextlib
import io
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import stock_runtime

spec = importlib.util.spec_from_file_location('bootstrap', Path(__file__).with_name('bootstrap-stock-runtime.py'))
bootstrap = importlib.util.module_from_spec(spec); spec.loader.exec_module(bootstrap)


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.destination = self.root / 'output'
        self.content = b'known stock library'
        self.entries = {'libc.so.0': {'file': 'lib/libuClibc-stock.so', 'sha256': hashlib.sha256(self.content).hexdigest()}}
        for module in (bootstrap, stock_runtime):
            mock = patch.object(module, 'inventory', return_value=self.entries)
            mock.start(); self.addCleanup(mock.stop)

    def fetch(self, name, entry, target): target.write_bytes(self.content)

    def test_publish_and_idempotent_reuse(self):
        self.assertTrue(bootstrap.bootstrap(self.destination, self.fetch))
        self.assertEqual((self.destination / 'lib/libc.so.0').read_bytes(), self.content)
        self.assertFalse(bootstrap.bootstrap(self.destination, lambda *_: self.fail('must not fetch again')))

    def test_hash_mismatch_does_not_publish(self):
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            bootstrap.bootstrap(self.destination, lambda n, e, p: p.write_bytes(b'wrong'))
        self.assertFalse(self.destination.exists())
        self.assertFalse(list(self.root.glob('.stock-runtime-*')))

    def test_late_failure_does_not_publish_earlier_files(self):
        self.entries['libm.so.0'] = dict(self.entries['libc.so.0'])
        def fetch(name, entry, target):
            target.write_bytes(self.content if name == 'libc.so.0' else b'wrong')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            bootstrap.bootstrap(self.destination, fetch)
        self.assertFalse(self.destination.exists())

    def test_manifest_path_cannot_become_a_remote_command(self):
        self.entries['../unsafe'] = self.entries.pop('libc.so.0')
        with self.assertRaisesRegex(ValueError, 'Invalid library name'):
            bootstrap.bootstrap(self.destination, lambda *_: self.fail('must reject before fetch'))

    def test_fetch_failure_does_not_publish(self):
        def fail(*_): raise ValueError('SSH failed')
        with self.assertRaisesRegex(ValueError, 'SSH failed'):
            bootstrap.bootstrap(self.destination, fail)
        self.assertFalse(self.destination.exists())

    def test_existing_mismatched_runtime_is_preserved(self):
        (self.destination / 'lib').mkdir(parents=True)
        library = self.destination / 'lib/libc.so.0'; library.write_bytes(b'old runtime')
        with self.assertRaises(ValueError): bootstrap.bootstrap(self.destination, self.fetch)
        self.assertEqual(library.read_bytes(), b'old runtime')

    def test_existing_empty_root_keeps_other_files(self):
        self.destination.mkdir(); marker = self.destination / 'keep'; marker.write_text('keep')
        bootstrap.bootstrap(self.destination, self.fetch)
        self.assertEqual(marker.read_text(), 'keep')

    def test_local_versioned_file_and_flat_reimport(self):
        source = self.root / 'source'; (source / 'lib').mkdir(parents=True)
        (source / 'lib/libuClibc-stock.so').write_bytes(self.content)
        bootstrap.bootstrap(self.destination, bootstrap.local_fetch(source))
        other = self.root / 'other'
        bootstrap.bootstrap(other, bootstrap.local_fetch(self.destination))
        self.assertEqual((other / 'lib/libc.so.0').read_bytes(), self.content)

    def test_local_symlink_escape_is_rejected(self):
        source = self.root / 'source'; (source / 'lib').mkdir(parents=True)
        outside = self.root / 'outside'; outside.write_bytes(self.content)
        (source / 'lib/libc.so.0').symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'missing from local extraction'):
            bootstrap.bootstrap(self.destination, bootstrap.local_fetch(source))
        self.assertFalse(self.destination.exists())


class BootstrapCliTests(unittest.TestCase):
    def invoke(self, argv, config):
        with patch.object(bootstrap, 'settings', return_value=config), \
             patch.object(bootstrap, 'bootstrap', return_value=False) as install, \
             patch.object(bootstrap, 'remote_fetch') as remote, \
             patch.object(bootstrap, 'local_fetch') as local, \
             patch.object(bootstrap, 'verify') as verify, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            bootstrap.main(argv)
        return install, remote, local, verify

    def test_no_flags_uses_configured_gateway_and_destination(self):
        install, remote, _, _ = self.invoke([], {'GATEWAY_IP': '192.0.2.1', 'STOCK_ROOTFS': '/tmp/runtime'})
        remote.assert_called_once_with('192.0.2.1', None)
        self.assertEqual(install.call_args.args[0], Path('/tmp/runtime'))

    def test_explicit_host_overrides_configured_gateway(self):
        _, remote, _, _ = self.invoke(['--host', '192.0.2.2'], {'GATEWAY_IP': '192.0.2.1', 'STOCK_ROOTFS': '/tmp/runtime'})
        remote.assert_called_once_with('192.0.2.2', None)

    def test_password_file_override_with_implicit_gateway(self):
        _, remote, _, _ = self.invoke(['--password-file', '/tmp/password'], {'GATEWAY_IP': '192.0.2.1', 'STOCK_ROOTFS': '/tmp/runtime'})
        remote.assert_called_once_with('192.0.2.1', Path('/tmp/password'))

    def test_local_source_does_not_contact_configured_gateway(self):
        _, remote, local, _ = self.invoke(['--from-root', '/tmp/source'], {'GATEWAY_IP': '192.0.2.1', 'STOCK_ROOTFS': '/tmp/runtime'})
        remote.assert_not_called()
        local.assert_called_once_with(Path('/tmp/source'))

    def test_verify_does_not_contact_configured_gateway(self):
        install, remote, _, verify = self.invoke(['--verify-only'], {'GATEWAY_IP': '192.0.2.1', 'STOCK_ROOTFS': '/tmp/runtime'})
        install.assert_not_called(); remote.assert_not_called()
        verify.assert_called_once_with(Path('/tmp/runtime'))

    def test_missing_or_invalid_configuration_does_not_fetch(self):
        for host in ('', '-invalid'):
            with patch.object(bootstrap, 'settings', return_value={'GATEWAY_IP': host}), \
                 patch.object(bootstrap, 'remote_fetch') as remote, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as result: bootstrap.main([])
                self.assertEqual(result.exception.code, 2)
                remote.assert_not_called()


if __name__ == '__main__': unittest.main()
