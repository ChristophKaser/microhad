#!/usr/bin/env python3
"""Harmless CPCd CLI and TCP startup test; the host peer does not drive a radio."""
import hashlib
import argparse
import json
from pathlib import Path
import socket
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--handshake", action="store_true", help="Attempt negotiation with the silent host peer")
args = parser.parse_args()
HOST = "192.168.178.62"
SSH = ["sshpass", "-f", str(ROOT.parent / "ssh_password.txt"), "ssh",
       "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=yes",
       "-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa",
       "-o", "PreferredAuthentications=password", "-o", "PubkeyAuthentication=no", f"root@{HOST}"]
token = str(time.time_ns())
binary = f"/tmp/cpcd-smoke-{token}"
folder = f"/tmp/cpc-smoke-{token}"
data = (ROOT / "build/cpc-stock/cpcd").read_bytes()
transfer = subprocess.run(SSH + [f"test ! -e {binary} && cat > {binary} && chmod 700 {binary} && md5sum {binary}"],
                          input=data, capture_output=True, timeout=20)
transfer.check_returncode()
assert transfer.stdout.decode().split()[0] == hashlib.md5(data).hexdigest()
route = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
route.connect((HOST, 9))
address = route.getsockname()[0]
route.close()
listener = socket.socket()
listener.bind((address, 0))
listener.listen(1)
listener.settimeout(12)
port = listener.getsockname()[1]
received = bytearray()
peer_error = []
connected = threading.Event()


def peer():
    try:
        conn, remote = listener.accept()
        with conn:
            assert remote[0] == HOST
            connected.set()
            conn.settimeout(12)
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                received.extend(chunk)
                # Deliberately silent: no complete CPC secondary is emulated.
    except Exception as error:
        peer_error.append(str(error))


worker = threading.Thread(target=peer, daemon=True)
worker.start()
script = f"""set -u
ulimit -c 0 || exit 1
mkdir {folder} || exit 2
bounded_cli() {{
    {binary} "$1" &
    child=$!
    (sleep 10; kill -9 "$child" 2>/dev/null) &
    watchdog=$!
    wait "$child"
    status=$?
    kill "$watchdog" 2>/dev/null
    wait "$watchdog" 2>/dev/null
    return "$status"
}}
bounded_cli --version || exit 1
bounded_cli --help || exit 1
cat > {folder}/cpcd.conf <<'CONFIG'
instance_name: cpc-smoke-{token}
bus_type: TCP
tcp_server_address: {address}
tcp_server_port: {port}
disable_encryption: true
reset_sequence: {str(args.handshake).lower()}
bootloader_recovery_pins_enabled: false
stdout_trace: true
trace_to_file: false
enable_frame_trace: true
rlimit_nofile: 256
binding_key_file: {folder}/unused-binding.key
traces_folder: {folder}/traces
CONFIG
{binary} -c {folder}/cpcd.conf > {folder}/startup.log 2>&1 &
child=$!
(sleep 15; kill -9 "$child" 2>/dev/null) &
watchdog=$!
trap 'kill "$child" "$watchdog" 2>/dev/null' HUP INT TERM EXIT
sleep 5
cat {folder}/startup.log
if kill -0 "$child" 2>/dev/null; then
    awk '/^(Name|State|VmSize|VmRSS|Threads):/' /proc/$child/status
    echo DAEMON_ALIVE
else
    wait "$child"
    status=$?
    echo DAEMON_EXIT "$status"
    test '{str(args.handshake).lower()}' = true || exit 1
fi
if test '{str(args.handshake).lower()}' = false; then
    test -S /tmp/cpcd/cpc-smoke-{token}/ctrl.cpcd.sock || exit 1
    echo PASS_LOCAL_CONTROL_SOCKET
fi
kill "$child"
sleep 1
kill -9 "$child" 2>/dev/null
wait "$child" 2>/dev/null
kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null
trap - HUP INT TERM EXIT
echo "CPCd test finished; physical radio untouched"
"""
try:
    run = subprocess.run(SSH + ["sh -s"], input=script, text=True, capture_output=True, timeout=35)
    print(run.stdout)
    print(run.stderr)
    worker.join(3)
    report = {"device_binary": binary, "device_test_directory": folder,
              "md5": hashlib.md5(data).hexdigest(), "exit": run.returncode,
              "output": run.stdout, "error_output": run.stderr,
              "received_bytes": len(received), "received_hex": received.hex(), "peer_error": peer_error}
    report["handshake_requested"] = args.handshake
    report["host_connected"] = connected.is_set()
    (ROOT / "logs" / f"cpc-smoke-{token}.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Host received {len(received)} CPC bytes; report logs/cpc-smoke-{token}.json")
    raise SystemExit(run.returncode or (1 if not connected.is_set() or (args.handshake and not received) else 0))
finally:
    listener.close()
