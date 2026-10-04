#!/usr/bin/env python3
"""Synthetic safety tests for Matter backup archive preflight."""
import io
import pathlib
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest

from matter_archive import ArchiveError, extract_staged, preflight


def write_archive(path, entries):
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name, kind, data in entries:
            info = tarfile.TarInfo(name)
            if kind == "directory":
                info.type = tarfile.DIRTYPE
                info.size = 0
                archive.addfile(info)
            elif kind == "symlink":
                info.type = tarfile.SYMTYPE
                info.linkname = data
                archive.addfile(info)
            else:
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))


class MatterArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="matter-archive-")
        self.path = pathlib.Path(self.temporary.name) / "state.tar"

    def tearDown(self):
        self.temporary.cleanup()

    def complete(self):
        return [("./", "directory", b""),
                ("./credentials.bin", "file", b"synthetic credentials"),
                ("./tmp/", "directory", b""),
                ("./tmp/settings.data", "file", b"synthetic Thread settings")]

    def test_accepts_complete_private_state_layout(self):
        write_archive(self.path, self.complete())
        self.assertEqual(preflight(self.path), (4, 46))

    def test_cli_preflights_without_printing_member_or_credential_contents(self):
        write_archive(self.path, self.complete())
        tool = str(pathlib.Path(__file__).with_name("matter_archive.py"))
        result = subprocess.run([sys.executable, tool, str(self.path)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("4 entries, 46 bytes", result.stdout)
        self.assertNotIn("credentials.bin", result.stdout)
        self.assertNotIn("synthetic credentials", result.stdout)
        destination = pathlib.Path(self.temporary.name) / "cli-stage"
        result = subprocess.run([sys.executable, tool, str(self.path), "--extract-to", str(destination)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Matter archive staged", result.stdout)
        self.assertNotIn("credentials.bin", result.stdout)
        self.assertNotIn("synthetic credentials", result.stdout)
        self.assertEqual(stat.S_IMODE((destination / "credentials.bin").stat().st_mode), 0o600)

    def test_rejects_incomplete_state_and_preserves_archive(self):
        entries = [entry for entry in self.complete() if entry[0] != "./tmp/"]
        write_archive(self.path, entries)
        original = self.path.read_bytes()
        with self.assertRaises(ArchiveError):
            preflight(self.path)
        self.assertEqual(self.path.read_bytes(), original)

    def test_extracts_new_private_staging_tree_with_restricted_modes(self):
        write_archive(self.path, self.complete())
        original = self.path.read_bytes()
        destination = pathlib.Path(self.temporary.name) / "staged"
        self.assertEqual(extract_staged(self.path, destination), (4, 46))
        self.assertEqual(self.path.read_bytes(), original)
        self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((destination / "tmp").stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((destination / "credentials.bin").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((destination / "tmp/settings.data").stat().st_mode), 0o600)
        self.assertEqual((destination / "credentials.bin").read_bytes(), b"synthetic credentials")

    def test_failed_or_existing_destination_is_preserved(self):
        destination = pathlib.Path(self.temporary.name) / "existing"
        destination.mkdir(mode=0o700)
        marker = destination / "keep"
        marker.write_text("preserve")
        write_archive(self.path, self.complete())
        with self.assertRaises(ArchiveError):
            extract_staged(self.path, destination)
        self.assertEqual(marker.read_text(), "preserve")

        entries = [entry for entry in self.complete() if entry[0] != "./credentials.bin"]
        write_archive(self.path, entries)
        missing = pathlib.Path(self.temporary.name) / "missing"
        with self.assertRaises(ArchiveError):
            extract_staged(self.path, missing)
        self.assertFalse(missing.exists())

    def test_rejects_traversal_links_duplicate_and_noncanonical_paths(self):
        bad_entries = [
            self.complete() + [("../escape", "file", b"x")],
            self.complete() + [("./credentials.bin", "file", b"duplicate")],
            self.complete() + [("./tmp/link", "symlink", "../../escape")],
            self.complete() + [("./tmp/../escape", "file", b"x")],
        ]
        for entries in bad_entries:
            with self.subTest(entries=entries[-1][0]):
                write_archive(self.path, entries)
                with self.assertRaises(ArchiveError):
                    preflight(self.path)

    def test_rejects_file_directory_conflict_and_oversized_archive(self):
        entries = self.complete() + [("./credentials.bin/child", "file", b"x")]
        write_archive(self.path, entries)
        with self.assertRaises(ArchiveError):
            preflight(self.path)
        self.path.write_bytes(b"x" * (64 * 1024 * 1024 + 1))
        with self.assertRaises(ArchiveError):
            preflight(self.path)


if __name__ == "__main__":
    unittest.main()
