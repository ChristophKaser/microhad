#!/usr/bin/env python3
"""Offline fixture checks for missing attribution and stale release inputs."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import tarfile
import unittest
from unittest.mock import patch
import zipfile
import notices

spec = importlib.util.spec_from_file_location('android_notices', Path(__file__).with_name('android-notices.py'))
android = importlib.util.module_from_spec(spec)
spec.loader.exec_module(android)


class Notices(unittest.TestCase):
    def test_deduplication_preserves_distinct_copyrights_and_escapes_html(self):
        records = [{'name': name, 'license': 'MIT', 'authors': ['Example author'], 'texts': [('LICENSE', text)]}
                   for name, text in [('a', 'Copyright Alice'), ('b', 'Copyright Bob'), ('c', 'Copyright Alice')]]
        text = notices.render(records)
        self.assertEqual(text.count('Copyright Alice'), 1)
        self.assertEqual(text.count('Copyright Bob'), 1)
        self.assertEqual(text, notices.render(list(reversed(records))))
        records[0]['texts'].append(('<script>', '<script>'))
        page = notices.page(records)
        self.assertIn('&lt;script&gt;', page)
        self.assertNotIn('<script>', page)
        self.assertIn('<strong>a</strong>', page)
        self.assertIn('Example author', page)
        self.assertEqual(page.count('Copyright Alice'), 2)
        self.assertEqual(page.count('<details>'), 4)
        self.assertNotIn('THIRD_PARTY.md', page)
        self.assertNotIn('Listing SDK', page)
        self.assertNotIn(hashlib.sha256(b'Copyright Alice').hexdigest(), page)
        self.assertNotIn('runtime-licenses.html', page)
        self.assertIn('runtime-licenses.html', notices.page(records, runtime_links=True))

    def test_snapshot_rejects_stale_inputs_and_tampered_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'third-party').mkdir()
            (root / 'third-party/supplements.json').write_text('{}')
            records = b'[]'
            text = notices.render([])
            (root / 'third-party/gateway-records.json').write_bytes(records)
            (root / 'third-party/gateway-notices.txt').write_text(text)
            snapshot = root / 'third-party/gateway-inputs.json'
            snapshot.write_text(json.dumps({'inputs': {'lock': 'old'}, 'sha256': hashlib.sha256(text.encode()).hexdigest(), 'records_sha256': hashlib.sha256(records).hexdigest()}))
            with patch.object(notices, 'ROOT', root), patch.object(notices, 'inputs', return_value={'lock': 'new'}):
                with self.assertRaisesRegex(ValueError, 'stale'):
                    notices.check_snapshot()
            with patch.object(notices, 'ROOT', root), patch.object(notices, 'inputs', return_value={'lock': 'old'}):
                self.assertEqual(notices.check_snapshot(), text)
                (root / 'third-party/gateway-records.json').write_bytes(b'[{}]')
                with self.assertRaisesRegex(ValueError, 'structured notices'):
                    notices.check_snapshot()
                (root / 'third-party/gateway-records.json').write_bytes(records)
                (root / 'third-party/gateway-notices.txt').write_text('tampered')
                with self.assertRaisesRegex(ValueError, 'checksum'):
                    notices.check_snapshot()

    def test_production_graph_uses_target_and_excludes_dev_and_host_macros(self):
        output = 'runtime v1.2.3\nshared v2.0.0 (*)\n'
        with patch.object(notices.subprocess, 'check_output', return_value=output) as run:
            self.assertEqual(notices.production_packages(), {('runtime', '1.2.3'), ('shared', '2.0.0')})
        for call in run.call_args_list:
            command = call.args[0][-1]
            self.assertIn('normal,no-proc-macro', command)
            self.assertIn('--locked', command)
            self.assertIn('--offline', command)
            self.assertIn('mips-unknown-linux-uclibc', command)
        self.assertIn('controller-service', run.call_args_list[0].args[0][-1])
        self.assertIn('ziggurat-server', run.call_args_list[1].args[0][-1])
        self.assertIn('patch.crates-io.nix.path', run.call_args_list[1].args[0][-1])

    def test_collection_excludes_unselected_lock_packages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'build/license-crates').mkdir(parents=True)
            (root / 'microhad/webui').mkdir(parents=True)
            (root / 'microhad/webui/package-lock.json').write_text('{"packages":{}}')
            for vendor in ('nanopb', 'acetimec'):
                folder = root / 'third-party' / vendor
                folder.mkdir(parents=True)
                (folder / 'LICENSE').write_text('Copyright vendor')
            (root / 'third-party/acetimec/UPSTREAM.json').write_text(
                '{"revision":"pinned-acetime","tzdb":"2025b"}')
            archive = root / 'build/license-crates/runtime-1.0.0.crate'
            with tarfile.open(archive, 'w') as tf:
                for name, text in [('Cargo.toml', '[package]\nname="runtime"\nversion="1.0.0"\nlicense="MIT"\nauthors=["Alice"]'),
                                   ('LICENSE', 'Copyright Alice')]:
                    data = text.encode()
                    member = tarfile.TarInfo('runtime-1.0.0/' + name)
                    member.size = len(data)
                    tf.addfile(member, io.BytesIO(data))
            checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
            (root / 'Cargo.lock').write_text(
                '[[package]]\nname="runtime"\nversion="1.0.0"\n'
                'source="registry+https://github.com/rust-lang/crates.io-index"\n'
                f'checksum="{checksum}"\n'
                '[[package]]\nname="test-only"\nversion="1.0.0"\nsource="git+unreviewed"\n')
            with patch.object(notices, 'ROOT', root), patch.object(notices, 'LOCKS', ['Cargo.lock']), \
                 patch.object(notices, 'SUBMODULES', {}), patch.object(notices, 'inputs', return_value={}), \
                 patch.object(notices, 'production_packages', return_value={('runtime', '1.0.0')}), \
                 patch.object(notices, 'authors', return_value=['Alice']):
                records = notices.collect_gateway()
            self.assertEqual([r['name'] for r in records], [
                'acetimec (restricted TZDB 2025b)', 'nanopb 0.4.9.1', 'runtime 1.0.0'])
            self.assertEqual(records[-1]['texts'], [('LICENSE', 'Copyright Alice')])
            self.assertEqual(records[0]['revision'], 'pinned-acetime')

    def test_checked_inventory_is_production_only_and_complete(self):
        records = json.loads((notices.ROOT / 'third-party/gateway-records.json').read_text())
        names = [record['name'] for record in records]
        self.assertIn('react 18.3.1', names)
        self.assertIn('mbedtls', names)
        self.assertIn('openthread', names)
        for name in names:
            self.assertNotRegex(name, r'gecko-sdk|rtl8196e|proptest|criterion|trybuild|probe|typescript|vite ')
        self.assertTrue(all(record['authors'] and record['texts'] for record in records))
        self.assertEqual(notices.render(records), (notices.ROOT / 'third-party/gateway-notices.txt').read_bytes().decode())

    def test_missing_license_text_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'No license text'):
                notices.texts(Path(directory))

    def test_nested_android_jar_notices_survive(self):
        inner, outer = io.BytesIO(), io.BytesIO()
        with zipfile.ZipFile(inner, 'w') as jar:
            jar.writestr('META-INF/NOTICE.txt', 'Original attribution')
        with zipfile.ZipFile(outer, 'w') as aar:
            aar.writestr('classes.jar', inner.getvalue())
            aar.writestr('res/raw/third_party_licenses', 'SDK bundled notices')
        texts = android.embedded(outer.getvalue())
        self.assertIn(('classes.jar/META-INF/NOTICE.txt', 'Original attribution'), texts)
        self.assertIn(('res/raw/third_party_licenses', 'SDK bundled notices'), texts)

    def test_inherited_maven_license_and_missing_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            child = cache / 'child.pom'
            child.write_text('<project><parent><groupId>test</groupId><artifactId>parent</artifactId><version>1</version></parent></project>')
            with self.assertRaisesRegex(ValueError, 'Missing parent POM'):
                android.pom_metadata(child, cache)
            parent = cache / 'test/parent/1/hash/parent.pom'
            parent.parent.mkdir(parents=True)
            parent.write_text('<project><licenses><license><name>Apache-2.0</name><url>https://www.apache.org/licenses/LICENSE-2.0</url></license></licenses></project>')
            self.assertEqual(android.pom_metadata(child, cache)[0][0][0], 'Apache-2.0')

    def test_unreviewed_android_license_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'LICENSE').write_text('Original license')
            pom = root / 'test.pom'
            pom.write_text('<project><licenses><license><name>Unknown terms</name></license></licenses></project>')
            jar = root / 'test.jar'
            with zipfile.ZipFile(jar, 'w'):
                pass
            with patch.object(android, 'ROOT', root):
                with self.assertRaisesRegex(ValueError, 'Unreviewed Android license'):
                    android.collect([{'coordinate': 'test:test:1', 'pom': str(pom), 'artifact': str(jar)}], root)


if __name__ == '__main__':
    unittest.main()
