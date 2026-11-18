#!/usr/bin/env python3
"""Authenticate gateway Matter UDP captures with keys recorded in the same run."""
import argparse
import collections
import ipaddress
import json
import os
import re
import struct
from pathlib import Path

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

from read_capture import blocks

KEY = re.compile(
    rb"SESSION_KEY direction=(RX|TX) session_id=([\dA-Fa-f]+) "
    rb"source_node=([\dA-Fa-f]+) peer=.*?\[([^]]+)\]:(\d+) key=\[([^]]+)\]"
)


def records(paths):
    pending = {}
    for path in paths:
        for data in blocks(path):
            if not data.startswith(b"GPC1\x02") or len(data) < 48 or data[5] not in (1, 2, 4):
                continue
            run, sequence, timestamp, length, offset = struct.unpack_from("<QQQII", data, 8)
            if not sequence or not 0 <= offset < length <= 4096 or offset % 1152:
                continue
            chunk_length = min(1152, length - offset)
            if len(data) < 48 + chunk_length:
                continue
            descriptor = data[5], timestamp, length
            existing, chunks = pending.setdefault((run, sequence), (descriptor, {}))
            if existing == descriptor:
                chunks[offset] = data[48:48 + chunk_length]
    for (run, sequence), ((kind, timestamp, length), chunks) in sorted(pending.items()):
        if len(chunks) == (length + 1151) // 1152:
            yield run, sequence, kind, timestamp, b"".join(chunks[offset] for offset in sorted(chunks))


def decrypt(paths, output=None):
    keys = {}
    counts = collections.Counter()
    for run, sequence, kind, timestamp, body in records(paths):
        if kind == 4:
            match = KEY.fullmatch(body)
            if match:
                direction, sid, node, addr, port, text = match.groups()
                try:
                    peer = (ipaddress.IPv6Address(addr.decode()).packed, int(port))
                    key = bytes.fromhex(text.decode().replace(",", " "))
                except ValueError:
                    continue
                if len(key) == 16:
                    keys[run, direction, peer, int(sid, 16)] = int(node, 16), key
                    counts["key_records"] += 1
            continue
        if len(body) < 30:
            counts["short_records"] += 1
            continue
        packet = body[22:]
        flags, sid, security = struct.unpack_from("<BHB", packet)
        if not sid:
            counts["unsecured"] += 1
            continue
        # Optional message extensions, privacy headers and group keys are outside
        # this gateway's captured unicast session-key format.
        if flags & ~7 or flags & 3 == 3 or security & ~0x40:
            counts["unsupported_header"] += 1
            continue
        aad_size = 8 + (8 if flags & 4 else 0) + {0: 0, 1: 8, 2: 2}[flags & 3]
        if len(packet) < aad_size + 16:
            counts["short_records"] += 1
            continue
        direction = b"RX" if kind == 1 else b"TX"
        peer = body[:16], struct.unpack_from("<H", body, 16)[0]
        material = keys.get((run, direction, peer, sid))
        if material is None:
            counts["missing_key"] += 1
            continue
        source_node, key = material
        nonce = packet[3:8] + source_node.to_bytes(8, "little")
        try:
            plaintext = AESCCM(key, tag_length=16).decrypt(nonce, packet[aad_size:], packet[:aad_size])
        except InvalidTag:
            counts["authentication_failed"] += 1
            continue
        counts["authenticated_" + direction.decode().lower()] += 1
        if output is not None:
            output.write(json.dumps(dict(run=run, sequence=sequence, timestamp_us=timestamp,
                                         direction=direction.decode(), session_id=sid,
                                         plaintext_hex=plaintext.hex())) + "\n")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, help="write authenticated plaintext JSONL to a new private file")
    args = parser.parse_args()
    if args.output:
        os.umask(0o077)
        with args.output.open("x") as output:
            result = decrypt(args.archives, output)
    else:
        result = decrypt(args.archives)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
