#!/usr/bin/env python3
"""Synthetic package/installer checks. No SSH connections or real credentials."""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/release'))
sys.path.insert(0, str(ROOT / 'scripts'))
import common
import install
import fetch
import release_source
spec = importlib.util.spec_from_file_location('release_builder', ROOT / 'scripts/build-release.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def options(**overrides):
    values = dict(action='check', mode='paired', host='192.0.2.1', ssh_port=22,
                  password_file=Path('/private/test-password'), config=None, ziggurat_state=None, rollback_dir=None)
    return argparse.Namespace(**dict(values, **overrides))


class Packages(unittest.TestCase):
    def test_release_locks_allow_only_reviewed_local_patch_substitutions(self):
        original = [{'name': 'nix', 'version': '0.26.4', 'dependencies': ['libc'],
                     'source': 'registry+https://github.com/rust-lang/crates.io-index', 'checksum': 'verified'}]
        patched = [{'name': 'nix', 'version': '0.26.4', 'dependencies': ['libc']}]
        builder.verify_lock_graph(original, patched, {('nix', '0.26.4')})
        with self.assertRaisesRegex(ValueError, 'dependency graph'):
            builder.verify_lock_graph(original, patched)
        with self.assertRaisesRegex(ValueError, 'dependency graph'):
            builder.verify_lock_graph(original, [dict(patched[0], dependencies=['new-dependency'])], {('nix', '0.26.4')})
        with self.assertRaisesRegex(ValueError, 'dependency graph'):
            builder.verify_lock_graph(original, [dict(patched[0], version='0.26.5')], {('nix', '0.26.4')})

    def test_git_fetch_preserves_export_ignored_and_substituted_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'upstream'
            subprocess.run(['git', 'init', '-q', str(source)], check=True)
            (source / '.gitattributes').write_text('.tag export-subst\nhidden export-ignore\n')
            (source / '.tag').write_text('$Format:%H$\n')
            (source / 'hidden').write_text('required committed file\n')
            subprocess.run(['git', '-C', str(source), 'add', '-A'], check=True)
            subprocess.run(['git', '-C', str(source), '-c', 'user.name=Fixture',
                            '-c', 'user.email=fixture@example.test', 'commit', '-qm', 'fixture'], check=True)
            revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
            real_run = subprocess.run
            def run(command, **kwargs):
                if 'fetch' in command:
                    command = list(command)
                    command[-2] = str(source)
                return real_run(command, **kwargs)
            record = dict(kind='git', destination='fetched', url='https://example.test/repo.git',
                          revision=revision, sha256=fetch.tree_digest(source))
            with patch.object(fetch.subprocess, 'run', side_effect=run):
                fetch.fetch_tree(root, record)
            self.assertEqual((root / 'fetched/.tag').read_text(), '$Format:%H$\n')
            self.assertTrue((root / 'fetched/hidden').is_file())

    def test_download_rejects_bad_cache_and_bad_server_content(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'source.tar'
            target.write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                fetch.download('https://example.test/source.tar', target, '0' * 64)
            target.unlink()
            with patch.object(fetch.urllib.request, 'urlopen', return_value=io.BytesIO(b'corrupt')):
                with self.assertRaisesRegex(ValueError, 'checksum'):
                    fetch.download('https://example.test/source.tar', target, '0' * 64)
            self.assertFalse(target.exists())
            self.assertFalse(target.with_suffix('.tar.part').exists())

    def test_fetch_archive_preserves_safe_links_and_rejects_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source.tar'
            with tarfile.open(source, 'w') as archive:
                file = tarfile.TarInfo('upstream/file')
                file.size = 4
                archive.addfile(file, io.BytesIO(b'data'))
                link = tarfile.TarInfo('upstream/link')
                link.type, link.linkname = tarfile.SYMTYPE, 'file/'
                archive.addfile(link)
            output = root / 'out'
            output.mkdir()
            fetch.extract(source, output, strip_root=True)
            self.assertEqual(os.readlink(output / 'link'), 'file/')
            with tarfile.open(source, 'w') as archive:
                archive.addfile(tarfile.TarInfo('../escape'))
            with self.assertRaises(tarfile.FilterError):
                fetch.extract(source, output)
            self.assertFalse((root / 'escape').exists())

    def test_patched_fetch_is_verified_atomic_and_does_not_rewrite_edits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'base').mkdir()
            (root / 'base/file').write_text('before\n')
            (root / 'expected').mkdir()
            (root / 'expected/file').write_text('after\n')
            (root / 'change.patch').write_text('diff --git a/file b/file\n--- a/file\n+++ b/file\n@@ -1 +1 @@\n-before\n+after\n')
            record = dict(kind='patched', destination='prepared', base='base', patch='change.patch',
                          sha256=fetch.tree_digest(root / 'expected'))
            (root / 'prepared').mkdir()  # Git archive's submodule placeholder.
            fetch.fetch_tree(root, record)
            self.assertEqual((root / 'prepared/file').read_text(), 'after\n')
            self.assertFalse((root / 'prepared/.git').exists())
            fetch.fetch_tree(root, record)
            (root / 'prepared/file').write_text('local edit\n')
            with self.assertRaisesRegex(ValueError, 'mismatch'):
                fetch.fetch_tree(root, record)
            self.assertEqual((root / 'prepared/file').read_text(), 'local edit\n')
            record['destination'] = 'failed'
            record['sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'mismatch'):
                fetch.fetch_tree(root, record)
            self.assertFalse((root / 'failed').exists())

    def test_source_bundle_verifies_pinned_fetched_trees(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / '.release').mkdir()
            (root / 'upstream').mkdir()
            (root / 'upstream/file').write_text('fetched source')
            (root / '.release/source-manifest.json').write_text('{"files": {}}')
            (root / '.release/fetch-manifest.json').write_text(json.dumps({'sources': [
                {'destination': 'upstream', 'sha256': fetch.tree_digest(root / 'upstream')}]}))
            with patch.object(release_source, 'ROOT', root):
                release_source.verify('upstream')
                (root / 'upstream/file').write_text('corrupt')
                with self.assertRaisesRegex(ValueError, 'mismatch'):
                    release_source.verify('upstream')

    def test_release_environment_disables_private_workspace_settings(self):
        env = dict(os.environ, WORKSPACE_CONFIG='/dev/null', TOOLCHAIN='crosstool-ng', LINK_MODE='static')
        value = subprocess.check_output(['sh', str(ROOT / 'scripts/workspace-env.sh'), '--get', 'TOOLCHAIN'], env=env, text=True)
        self.assertEqual(value.strip(), 'crosstool-ng')

    def test_inventory_detects_tamper_and_external_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'file').write_text('original')
            records = common.inventory(root)
            common.verify_inventory(root, records)
            (root / 'file').write_text('changed')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                common.verify_inventory(root, records)
            with self.assertRaisesRegex(ValueError, 'Unsafe'):
                common.verify_inventory(root, {'../file': records['file']})
            (root / 'link').symlink_to('/etc/passwd')
            with self.assertRaisesRegex(ValueError, 'External'):
                common.inventory(root)

    def test_archives_are_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'source'
            root.mkdir()
            (root / 'file').write_text('source')
            first, second = Path(directory) / 'a.tar.xz', Path(directory) / 'b.tar.xz'
            builder.archive(root, first, 12345)
            os.utime(root / 'file', (99999, 99999))
            builder.archive(root, second, 12345)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with tarfile.open(first) as archive:
                self.assertEqual(archive.getmember('source/file').mtime, 12345)
                self.assertEqual(archive.getmember('source/file').uid, 0)

    def test_source_export_excludes_untracked_private_files_and_history(self):
        with tempfile.TemporaryDirectory() as directory:
            root, output = Path(directory) / 'repo', Path(directory) / 'out'
            root.mkdir()
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'source.c').write_text('public')
            subprocess.run(['git', '-C', str(root), 'add', 'source.c'], check=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test', 'commit', '-qm', 'fixture'], check=True)
            (root / 'source.c').write_text('staged change must not leak')
            subprocess.run(['git', '-C', str(root), 'add', 'source.c'], check=True)
            (root / 'workspace.local.env').write_text('SSH_PASSWORD=private')
            builder.export_git(root, output, recursive=False, filter_root=True)
            self.assertEqual(sorted(p.name for p in output.iterdir()), ['source.c'])
            self.assertEqual((output / 'source.c').read_text(), 'public')
            self.assertTrue(builder.excluded('firmware/dependencies/gecko-sdk/secret'))
            self.assertTrue(builder.excluded('io.github.microhad/app/build.gradle'))
            self.assertTrue(builder.excluded('microhad/test/fixtures/tls/localhost.key'))
            self.assertFalse(builder.excluded('firmware/scripts/build-maintenance.sh'))

    def test_source_bundle_detects_missing_or_changed_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / '.release').mkdir()
            (root / 'source').mkdir()
            (root / 'source/file').write_text('original')
            records = common.inventory(root / 'source')
            records = {'source/' + key: value for key, value in records.items()}
            (root / '.release/source-manifest.json').write_text(json.dumps({'files': records}))
            with patch.object(release_source, 'ROOT', root):
                release_source.verify('source')
                with self.assertRaisesRegex(ValueError, 'no verified'):
                    release_source.verify('missing')
                (root / 'source/file').unlink()
                with self.assertRaisesRegex(ValueError, 'checksum'):
                    release_source.verify('source')

    def test_elf_rejects_dynamic_or_wrong_architecture(self):
        header = 'ELF32 big endian MIPS mips1 o32'
        with patch.object(common.subprocess, 'check_output', side_effect=[header, 'Soft float', '', 'There is no dynamic section']):
            common.verify_elf('fixture')
        with patch.object(common.subprocess, 'check_output', side_effect=[header, 'Soft float', 'INTERP']):
            with self.assertRaisesRegex(ValueError, 'static'):
                common.verify_elf('fixture')
        with patch.object(common.subprocess, 'check_output', side_effect=['ELF64 x86-64', '']):
            with self.assertRaisesRegex(ValueError, 'MIPS'):
                common.verify_elf('fixture')


class Installer(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / 'bin').mkdir()
        (self.root / 'bin/microhad').write_bytes(b'fixture binary')
        self.root_patch = patch.object(install, 'ROOT', self.root)
        self.root_patch.start()

    def tearDown(self):
        self.root_patch.stop()
        self.temporary.cleanup()

    def test_check_is_read_only_and_no_credential_environment_leaks(self):
        instance = install.Installer(options())
        with patch.object(instance, 'remote', return_value='mips\n999999\nconfigured\npaired') as remote:
            instance.preflight()
        command = remote.call_args.args[0]
        self.assertNotIn('dd of=', command)
        self.assertNotIn('kill ', command)
        self.assertNotIn('rm ', command)
        self.assertNotIn('SSH_PASSWORD', instance.env)
        self.assertEqual(instance.env['WORKSPACE_CONFIG'], '/dev/null')

    def test_existing_configuration_requires_update(self):
        instance = install.Installer(options(action='install'))
        with patch.object(instance, 'remote', return_value='mips\n999999\nconfigured\npaired'):
            with self.assertRaisesRegex(ValueError, 'use update'):
                instance.preflight()

    def test_backend_change_and_deleted_images_are_refused(self):
        instance = install.Installer(options(action='update', mode='microhad-only'))
        with patch.object(instance, 'remote', return_value='mips\n999999\nconfigured\npaired'):
            with self.assertRaisesRegex(ValueError, 'Backend change'):
                instance.preflight()
        with patch.object(instance, 'remote', return_value='mips\n999999\nconfigured\nmicrohad-only\ndeleted'):
            with self.assertRaisesRegex(ValueError, 'deleted'):
                instance.preflight()

    def test_flash_failure_is_before_upload(self):
        instance = install.Installer(options())
        with patch.object(instance, 'remote', return_value='mips\n1\nconfigured\npaired') as remote:
            with self.assertRaisesRegex(ValueError, 'flash'):
                instance.preflight()
        self.assertEqual(remote.call_count, 1)

    def test_upload_uses_checksum_reserve_and_no_clobber_for_state(self):
        instance = install.Installer(options())
        with patch.object(instance, 'remote', return_value='') as remote:
            instance.upload(self.root / 'bin/microhad', '/tuya/native/config.json', '600', absent=True)
        commands = [call.args[0] for call in remote.call_args_list]
        self.assertIn('test ! -e /tuya/native/config.json', commands[0])
        self.assertIn('md5sum', commands[-1])
        self.assertIn('-ge 512', commands[-1])
        self.assertIn('ln /tuya/native/config.json.release-new /tuya/native/config.json', commands[-1])
        self.assertNotIn('mv ', commands[-1])

    def test_failed_checksum_removes_temporary_file(self):
        instance = install.Installer(options())
        with patch.object(instance, 'remote', side_effect=['', '', subprocess.CalledProcessError(1, 'fixture'), '']) as remote:
            with self.assertRaises(subprocess.CalledProcessError):
                instance.upload(self.root / 'bin/microhad', '/tuya/native/microhad')
        self.assertEqual(remote.call_args.args[0], 'rm -f /tuya/native/microhad.release-new')

    def test_microhad_only_installs_no_ziggurat_matter_or_state(self):
        (self.root / 'webui').mkdir()
        (self.root / 'webui/index.html').write_text('fixture')
        (self.root / 'webui/licenses.html').write_text('licenses')
        instance = install.Installer(options(action='install', mode='microhad-only'))
        instance.fresh = True
        with patch.object(instance, 'check_radio_free'), patch.object(instance, 'upload') as upload, \
                patch.object(instance, 'remote') as remote, patch.object(instance, 'verify_dashboard'):
            instance.native()
        destinations = [call.args[1] for call in upload.call_args_list]
        self.assertIn('/tuya/native/microhad', destinations)
        self.assertIn('/tuya/native/webui/licenses.html', destinations)
        self.assertEqual(destinations[-1], '/tuya/tuya_start.sh')
        self.assertFalse(any('state' in path or 'matter' in path or 'ziggurat' in path for path in destinations))
        self.assertEqual(remote.call_count, 1)

    def test_native_failure_does_not_start_launcher(self):
        instance = install.Installer(options(action='install', mode='microhad-only'))
        instance.fresh = True
        with patch.object(instance, 'check_radio_free'), \
                patch.object(instance, 'upload', side_effect=subprocess.CalledProcessError(1, 'fixture')), \
                patch.object(instance, 'remote') as remote:
            with self.assertRaises(subprocess.CalledProcessError):
                instance.native()
        remote.assert_not_called()

    def test_native_readiness_checks_exact_executable_and_ezsp_backend(self):
        instance = install.Installer(options(mode='microhad-only'))
        response = subprocess.CompletedProcess([], 0, stdout=b'{"online":true,"coordinator_backend":"ezsp"}')
        with patch.object(instance, 'remote', return_value='running') as remote, \
                patch.object(install.subprocess, 'run', return_value=response) as run:
            instance.verify_dashboard()
        self.assertIn(" -> /tuya/native/microhad", remote.call_args.args[0])
        self.assertIn('/licenses.html', run.call_args.args[0][-1])

    def test_paired_update_delegates_without_configuration_or_identity(self):
        instance = install.Installer(options(action='update'))
        with patch.object(install.subprocess, 'run') as run:
            instance.paired()
        command = run.call_args.args[0]
        self.assertIn('--maintenance-deploy', command)
        self.assertIn('--rng-seed-helper', command)
        self.assertNotIn('--config', command)
        self.assertNotIn('--ziggurat-state', command)
        self.assertFalse(any('licenses/' in part for part in command))


if __name__ == '__main__':
    unittest.main()
