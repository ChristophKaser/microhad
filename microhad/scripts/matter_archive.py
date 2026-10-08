#!/usr/bin/env python3
"""Preflight an unencrypted Matter state tar archive without extracting it."""
import argparse
import os
import pathlib
import posixpath
import sys
import tarfile

MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_MEMBER_BYTES = 16 * 1024 * 1024
MAX_MEMBERS = 4096
MAX_TOTAL_BYTES = 64 * 1024 * 1024


class ArchiveError(ValueError):
    """The archive is unsafe, incomplete, or outside the supported bounds."""


def _canonical_name(name):
    if not name or "\\" in name or name.startswith("/"):
        raise ArchiveError("invalid path")
    normalized = posixpath.normpath(name)
    if normalized in ("", "."):
        return ""
    if normalized == ".." or normalized.startswith("../"):
        raise ArchiveError("path traversal")
    if normalized != name.rstrip("/").removeprefix("./"):
        raise ArchiveError("noncanonical path")
    return normalized


def preflight(path):
    archive_path = pathlib.Path(path)
    size = archive_path.stat().st_size
    if size < 1024 or size > MAX_ARCHIVE_BYTES:
        raise ArchiveError("archive size out of bounds")

    entries = {}
    total = 0
    try:
        with tarfile.open(archive_path, mode="r:") as archive:
            for member in archive:
                if len(entries) >= MAX_MEMBERS:
                    raise ArchiveError("too many members")
                name = _canonical_name(member.name)
                if name in entries:
                    raise ArchiveError("duplicate path")
                if member.pax_headers:
                    raise ArchiveError("extended path metadata is unsupported")
                if member.isdir():
                    if member.size != 0:
                        raise ArchiveError("invalid directory size")
                    kind = "directory"
                elif member.isreg():
                    if member.size < 0 or member.size > MAX_MEMBER_BYTES:
                        raise ArchiveError("member size out of bounds")
                    total += member.size
                    if total > MAX_TOTAL_BYTES:
                        raise ArchiveError("total size out of bounds")
                    kind = "file"
                else:
                    raise ArchiveError("links and special files are unsupported")
                if not name:
                    if kind != "directory":
                        raise ArchiveError("archive root is not a directory")
                    entries[name] = kind
                    continue
                entries[name] = kind
    except (tarfile.TarError, OSError, EOFError) as error:
        raise ArchiveError("archive cannot be read") from error

    if entries.get("credentials.bin") != "file" or entries.get("tmp") != "directory":
        raise ArchiveError("required state paths are missing")

    for name, kind in entries.items():
        if not name:
            continue
        parent = posixpath.dirname(name)
        while parent:
            if entries.get(parent, "directory") != "directory":
                raise ArchiveError("file conflicts with parent path")
            parent = posixpath.dirname(parent)
        if kind == "file" and any(other.startswith(name + "/") for other in entries):
            raise ArchiveError("file conflicts with child path")
    return len(entries), total


def _make_directories(root, relative):
    current = root
    for component in relative.split("/") if relative else ():
        current = current / component
        try:
            current.mkdir(mode=0o700)
        except FileExistsError:
            if current.is_symlink() or not current.is_dir():
                raise ArchiveError("directory conflicts with staged file")
        current.chmod(0o700)
    return current


def extract_staged(path, destination):
    """Extract only a preflighted archive into a new private staging directory."""
    count, total = preflight(path)
    target = pathlib.Path(destination)
    if not target.is_absolute():
        raise ArchiveError("staging path must be absolute")
    parent = target.parent
    try:
        if not parent.is_dir() or parent.resolve(strict=True) != parent.absolute():
            raise ArchiveError("staging parent must be a real directory")
        os.mkdir(str(target), 0o700)
    except FileExistsError as error:
        raise ArchiveError("staging destination already exists") from error
    except OSError as error:
        raise ArchiveError("cannot create private staging directory") from error

    try:
        with tarfile.open(path, mode="r:") as archive:
            for member in archive:
                name = _canonical_name(member.name)
                if not name:
                    continue
                output = target.joinpath(*name.split("/"))
                parent_relative = posixpath.dirname(name)
                _make_directories(target, parent_relative)
                if member.isdir():
                    _make_directories(target, name)
                    continue
                source = archive.extractfile(member)
                if source is None:
                    raise ArchiveError("cannot read regular archive member")
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
                flags |= getattr(os, "O_NOFOLLOW", 0)
                descriptor = os.open(str(output), flags, 0o600)
                try:
                    if not os.path.isfile(str(output)) or os.fstat(descriptor).st_nlink != 1:
                        raise ArchiveError("unsafe staged file")
                    remaining = member.size
                    while remaining:
                        chunk = source.read(min(65536, remaining))
                        if not chunk:
                            raise ArchiveError("truncated archive member")
                        view = memoryview(chunk)
                        while view:
                            written = os.write(descriptor, view)
                            if written <= 0:
                                raise ArchiveError("staged write failed")
                            view = view[written:]
                        remaining -= len(chunk)
                    if source.read(1):
                        raise ArchiveError("archive member exceeds declared size")
                    os.fchmod(descriptor, 0o600)
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
                source.close()
        for directory, _, _ in os.walk(str(target), topdown=False, followlinks=False):
            descriptor = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fchmod(descriptor, 0o700)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
    except (ArchiveError, OSError, tarfile.TarError, EOFError) as error:
        raise ArchiveError("staged extraction failed; private candidate preserved") from error
    return count, total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", help="private plaintext tar produced by successful decryption")
    parser.add_argument("--extract-to", help="new absolute private staging directory")
    args = parser.parse_args()
    try:
        count, total = (extract_staged(args.archive, args.extract_to) if args.extract_to
                        else preflight(args.archive))
    except (ArchiveError, OSError):
        print("Matter archive preflight failed", file=sys.stderr)
        return 1
    action = "staged" if args.extract_to else "preflight passed"
    print(f"Matter archive {action} ({count} entries, {total} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
