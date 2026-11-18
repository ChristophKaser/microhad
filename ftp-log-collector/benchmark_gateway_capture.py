#!/usr/bin/env python3
"""Replay synthetic traffic through a private collector on an explicitly selected gateway."""
import argparse
import re
import shlex
import socket
import secrets
import struct
import subprocess
from pathlib import Path

from test_collector import Server

ROOT = Path(__file__).resolve().parent.parent


def summarize(files, mode):
    if mode == "text":
        data = b"".join(value for name, value in files.items() if name.endswith(".log"))
        return len(set(re.findall(rb"record=(\d+) ", data))), len(data), ""
    records = {}
    reports = {}
    volume = 0
    for name, data in files.items():
        if not name.endswith(".pcapng"):
            continue
        volume += len(data)
        cursor = 0
        while cursor + 12 <= len(data):
            kind, size = struct.unpack_from("<II", data, cursor)
            if size < 12 or cursor + size > len(data):
                break
            body = data[cursor + 12:cursor + size - 4]
            if kind == 0x40000BAD and body.startswith(b"GPC1"):
                run, sequence, _, length, offset = struct.unpack_from("<QQQII", body, 8)
                if body[5] == 0:
                    reports["producer_drops"], reports["send_failures"] = struct.unpack_from("<QQ", body, 48)
                else:
                    total, chunks = records.setdefault((run, sequence), (length, set()))
                    chunks.add(offset)
            if kind == 0x40000BAD and body.startswith(b"GPR1"):
                reports["receiver_datagram_drops"] = struct.unpack_from("<I", body, 4)[0]
            cursor += size
    return sum(len(chunks) == (size + 1151) // 1152 for size, chunks in records.values()), volume, reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--collector", required=True, type=Path)
    parser.add_argument("--producer", required=True, type=Path)
    parser.add_argument("--matched-only", action="store_true")
    args = parser.parse_args()
    ssh_args = ["bash", str(ROOT / "scripts/workspace-ssh.sh"), "--", args.host]

    def ssh(command, data=None):
        return subprocess.run(ssh_args + [command], input=data, stdout=subprocess.PIPE,
                              check=True, timeout=60).stdout.decode().strip()

    route = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    route.connect((args.host, 9))
    server = Server(route.getsockname()[0])
    route.close()
    directory = None
    try:
        candidate = shlex.quote("/tmp/gateway-capture-bench." + secrets.token_hex(8))
        ssh(f"umask 077; mkdir {candidate}")
        directory = candidate
        for source, target in ((args.collector, "collector"), (args.producer, "replay")):
            ssh(f"cat > {directory}/{target} && chmod 700 {directory}/{target}", source.read_bytes())
        config = (
            f"ftp_host={server.host}\nftp_port={server.port}\nftp_user=fixture\n"
            "ftp_password=fixture\nftp_directory=/\nudp_port=45451\n"
            "capture_udp_port=45452\ntcp_port=45453\nrotate_seconds=1\nlog_files=100\n"
        )
        ssh(f"umask 077; cat > {directory}/config", config.encode())
        scenarios = (
            ("steady", 300, 10000, 0, 3), ("burst", 300, 0, 0, 3),
            ("slow FTP", 3000, 0, .2, 1),
        )
        if args.matched_only:
            scenarios = (("matched steady", 120, 50000, 0, 2),
                         ("matched burst", 30, 0, 0, 2))
        for scenario, count, pace, delay, repeats in scenarios:
            server.data_delay = delay
            for _ in range(repeats):
                for mode in ("off", "text", "udp"):
                    with server.lock:
                        server.files.clear()
                    port = 45451 if mode == "text" else 45452
                    result = ssh(
                        f"cd {directory} || exit 1; ./collector config & collector=$!; "
                        "trap 'kill $collector 2>/dev/null; wait $collector' EXIT; "
                        "sleep 1; kill -0 $collector || exit 1; "
                        f"./replay {mode} 127.0.0.1:{port} {count} {pace} || exit 1; "
                        "sleep 2; awk '{ print \"collector_ticks=\" $14+$15 }' /proc/$collector/stat; "
                        "grep VmHWM /proc/$collector/status || :"
                    )
                    with server.lock:
                        delivered, volume, drops = summarize(server.files, mode) if mode != "off" else (0, 0, "")
                    print(f"{scenario}: {result.replace(chr(10), ' ')} delivered={delivered} archive_bytes={volume} {drops}",
                          flush=True)
    finally:
        if directory:
            ssh(f"rm -rf {directory}")
        server.close()


if __name__ == "__main__":
    main()
