#!/usr/bin/env python3
"""Offline fixture checks for missing attribution and stale release inputs."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import notices

spec = importlib.util.spec_from_file_location('android_notices', Path(__file__).with_name('android-notices.py'))
android = importlib.util.module_from_spec(spec)
spec.loader.exec_module(android)


class Notices(unittest.TestCase):
    def test_deduplication_preserves_distinct_copyrights_and_escapes_html(self):
        records = [{'name': name, 'license': 'MIT', 'texts': [('LICENSE', text)]}
                   for name, text in [('a', 'Copyright Alice'), ('b', 'Copyright Bob'), ('c', 'Copyright Alice')]]
        text = notices.render(records)
        self.assertEqual(text.count('Copyright Alice'), 1)
        self.assertEqual(text.count('Copyright Bob'), 1)
        self.assertEqual(text, notices.render(list(reversed(records))))
        self.assertIn('&lt;script&gt;', notices.page('<script>'))
        self.assertNotIn('<script>', notices.page('<script>'))

    def test_snapshot_rejects_stale_inputs_and_tampered_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'third-party').mkdir()
            (root / 'third-party/supplements.json').write_text('{}')
            (root / 'third-party/gateway-notices.txt').write_text('verified')
            snapshot = root / 'third-party/gateway-inputs.json'
            snapshot.write_text(json.dumps({'inputs': {'lock': 'old'}, 'sha256': hashlib.sha256(b'verified').hexdigest()}))
            with patch.object(notices, 'ROOT', root), patch.object(notices, 'inputs', return_value={'lock': 'new'}):
                with self.assertRaisesRegex(ValueError, 'stale'):
                    notices.check_snapshot()
            with patch.object(notices, 'ROOT', root), patch.object(notices, 'inputs', return_value={'lock': 'old'}):
                self.assertEqual(notices.check_snapshot(), 'verified')
                (root / 'third-party/gateway-notices.txt').write_text('tampered')
                with self.assertRaisesRegex(ValueError, 'checksum'):
                    notices.check_snapshot()

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
