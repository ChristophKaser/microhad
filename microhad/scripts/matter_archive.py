#!/usr/bin/env python3
"""Preflight an unencrypted Matter state tar archive without extracting it."""
import argparse
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", help="private plaintext tar produced by successful decryption")
    args = parser.parse_args()
    try:
        count, total = preflight(args.archive)
    except (ArchiveError, OSError):
        print("Matter archive preflight failed", file=sys.stderr)
        return 1
    print(f"Matter archive preflight passed ({count} entries, {total} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
