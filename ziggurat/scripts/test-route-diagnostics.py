#!/usr/bin/env python3
"""Check route-loss diagnostics using authenticated synthetic neighbor traffic."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def reassemble(text):
    parts = {}
    entries = []
    for line in text.splitlines():
        body = line.split("\t", 2)[-1]
        match = re.fullmatch(r"\[([0-9a-f]{8})/([0-9a-f]{8})([+=])\] (.*)", body)
        if not match:
            entries.append(body)
            continue
        key, index, final, payload = match.groups()
        index = int(index, 16)
        if index == 0:
            parts[key] = []
        assert key in parts and len(parts[key]) == index, line
        parts[key].append(payload)
        if final == "=":
            entries.append("".join(parts.pop(key)))
    return "\n".join(entries)


def main():
    with tempfile.TemporaryDirectory(prefix="route-diagnostics-") as directory:
        path = Path(directory)
        shim = path / "capture.so"
        capture = path / "capture.log"
        subprocess.run([
            "cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "micro-thread-matter-daemon/tests/helpers/syslog-capture.c"),
            "-o", str(shim)], check=True)
        subprocess.run([
            "python3", str(Path(__file__).with_name("test-neighbor-recovery.py"))],
            env=dict(os.environ, LD_PRELOAD=str(shim),
                     MATTER_SYSLOG_CAPTURE=str(capture), RUST_LOG="info", NO_COLOR="1"),
            check=True, timeout=60)
        text = reassemble(capture.read_text())
        # The fixture installs a route before its next hop stops advertising us.
        assert re.search(
            r"ROUTE_INVALIDATED destination=Nwk\(0x3333\) "
            r"next_hop=Nwk\(0x2222\) reason=link_status_lost", text), text
        for before, after in ((0, 1), (1, 0)):
            assert re.search(
                r"NEIGHBOR_LINK node=Nwk\(0x2222\) ieee=Eui64\([^)]*\) "
                rf"outgoing_before={before} outgoing_after={after} "
                r"reason=link_status", text), text
        assert "outgoing_before=1 outgoing_after=1" not in text
        assert "dropped " not in text, "fixture lost diagnostic entries"
        print("PASS: info-level correlated route invalidation and neighbor transitions")


if __name__ == "__main__":
    main()
