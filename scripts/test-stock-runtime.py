#!/usr/bin/env python3
"""Check stock-only dependency enforcement and versioned ELF imports."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import stock_runtime

spec = importlib.util.spec_from_file_location('verifier', Path(__file__).with_name('verify-gateway-target.py'))
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class StockRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'lib').mkdir()
        self.library = self.root / 'lib/libc.so.0'
        self.library.write_bytes(b'stock fixture')
        self.values = {'STOCK_ROOTFS': self.root}
        self.entries = {'libc.so.0': {'sha256': hashlib.sha256(self.library.read_bytes()).hexdigest()}}
        mock = patch.object(stock_runtime, 'inventory', return_value=self.entries)
        mock.start()
        self.addCleanup(mock.stop)

    def test_exact_stock_library_and_unprovided_library(self):
        self.assertEqual(stock_runtime.resolve(self.values, 'libc.so.0'), self.library)
        with self.assertRaisesRegex(ValueError, 'not provided by stock image'):
            stock_runtime.resolve(self.values, 'libutil.so.0')

    def test_modified_or_missing_runtime(self):
        self.library.write_bytes(b'compiler SDK replacement')
        with self.assertRaisesRegex(ValueError, 'does not match recorded stock image'):
            stock_runtime.resolve(self.values, 'libc.so.0')
        self.library.unlink()
        with self.assertRaisesRegex(ValueError, 'stock runtime library missing'):
            stock_runtime.resolve(self.values, 'libc.so.0')

    def test_symlink_cannot_escape_runtime(self):
        self.library.unlink()
        self.library.symlink_to('/bin/true')
        with self.assertRaisesRegex(ValueError, 'stock runtime library missing'):
            stock_runtime.resolve(self.values, 'libc.so.0')

    def version_check(self, version):
        binary = self.root / 'executable'
        loader = self.root / 'lib/ld-uClibc.so.0'
        loader.write_bytes(b'loader fixture')
        def read(values, path, *args):
            path = Path(path)
            if args == ('-l',):
                return '[Requesting program interpreter: /lib/ld-uClibc.so.0]'
            if args == ('-h',):
                return 'ELF32 big endian MIPS mips1 o32'
            if args == ('-d',):
                return '(NEEDED) [libc.so.0]' if path == binary else ''
            if args == ('--dyn-syms', '--wide'):
                if path == binary:
                    return f'1: 00000000 0 FUNC GLOBAL DEFAULT [MIPS PLT] UND api@{version} (2)'
                if path == self.library:
                    return '1: 00001000 8 FUNC GLOBAL DEFAULT 4 api@@VER_1'
                return ''
            self.fail(str(args))
        with patch.object(verifier, 'read', side_effect=read), patch.object(
                verifier, 'resolve_runtime', side_effect=lambda values, name: self.root / 'lib' / name):
            return verifier.dynamic_dependencies(self.values, binary)

    def test_matching_symbol_version_and_mips_plt_annotation(self):
        self.version_check('VER_1')

    def test_wrong_symbol_version_is_unresolved(self):
        with self.assertRaisesRegex(ValueError, 'unresolved dynamic symbols: api@VER_2'):
            self.version_check('VER_2')


if __name__ == '__main__':
    unittest.main()
