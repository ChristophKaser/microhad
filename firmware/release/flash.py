#!/usr/bin/env python3
"""Validate or flash the bundled application-only EFR32MG1B232 firmware.

Host dependencies: bellows==1.0.2 universal-silabs-flasher==1.1.0.
No repository checkout, workspace settings or device-side Python is required.
"""
import argparse
import asyncio
import hashlib
import importlib.metadata
import ipaddress
from pathlib import Path
import re
import secrets
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request

IMAGE_SHA256 = "@IMAGE_SHA256@"
MIGRATION_URL = "https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/ziggurat-deployment-plan.md"
BRIDGE_URL = "https://github.com/banksy-git/lidl-gateway-freedom/releases/download/Release-1.2/serialgateway"
BRIDGE_SHA256 = "1321510921d97b59336f7e6dc80cc3e8b8eccb2b804c940ab264c666aec3e827"
WARNING = ("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
           "WARNING: REPLACING EZSP FIRMWARE WITH HDLC\n"
           "Your Zigbee network will probably need to be recreated unless you\n"
           "complete the Ziggurat network migration BEFORE flashing. This script\n"
           "does not export or migrate your network. Keep private recovery backups.\n"
           "Do not reboot into the old EZSP coordinator after replacement.\n"
           "Migration: " + MIGRATION_URL + "\n"
           "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n")

# Inspect actual character-device identities, not just symlink spellings. Output
# only process identities, never arbitrary command lines/configuration or keys.
SCAN = r'''
set -eu
[ "$(awk '/^Uid:/ {print $2}' /proc/self/status)" = 0 ] && [ -c /dev/ttyS1 ] || exit 1
uart=$(ls -lnL /dev/ttyS1 | awk '{print $5 $6}')
case "$uart" in *,*) ;; *) exit 1;; esac
for directory in /proc/[0-9]*; do
    [ -d "$directory" ] || continue
    pid=${directory#/proc/}
    link=$(ls -l "$directory/exe" 2>/dev/null) || { [ ! -d "$directory" ] || [ -z "$(cat "$directory/cmdline")" ] || exit 1; continue; }
    case "$link" in *' -> '*) exe=${link##* -> };; *) [ ! -d "$directory" ] || [ -z "$(cat "$directory/cmdline")" ] || exit 1; continue;; esac
    identity=$exe
    case "$exe" in /bin/sh|/bin/ash|/bin/busybox)
        # Stock ash removes NULs in command substitutions. Match the complete
        # concatenated argv, never a substring (our own sh -c contains paths).
        argv=$(cat "$directory/cmdline")
        case "$argv" in
            /bin/sh/tuya/tuya_start.sh|sh/tuya/tuya_start.sh|/bin/ash/tuya/tuya_start.sh|ash/tuya/tuya_start.sh) identity=/tuya/tuya_start.sh;;
            /bin/sh/tuya/native/paired_start.sh|sh/tuya/native/paired_start.sh|/bin/ash/tuya/native/paired_start.sh|ash/tuya/native/paired_start.sh) identity=/tuya/native/paired_start.sh;;
        esac;;
    esac
    state=$(sed 's/.*) //' "$directory/stat" | awk '{print $2 " " $20 " " $1}') || { [ ! -d "$directory" ] || exit 1; continue; }
    owner=0
    [ -r "$directory/fd" ] && [ -x "$directory/fd" ] || { [ ! -d "$directory" ] || exit 1; continue; }
    for fd in "$directory"/fd/*; do
        [ -c "$fd" ] || continue
        device=$(ls -lnL "$fd" 2>/dev/null | awk '{print $5 $6}')
        case "$device" in *,*) ;; *) [ ! -e "$fd" ] || exit 1; continue;; esac
        [ "$device" != "$uart" ] || owner=1
    done
    case "$identity" in
        /tuya/native/microhad|/tuya/native/ziggurat|/tuya/native/mthread-matter|/tuya/serialgateway|/tmp/microhad-radio-release.*/serialgateway|/var/tmp/microhad-radio-release.*/serialgateway|/tuya/tuya_start.sh|/tuya/native/paired_start.sh) ;;
        *) [ "$owner" = 1 ] || continue;;
    esac
    printf '%s\t%s\t%s\t%s\n' "$pid" "$state" "$owner" "$identity"
done
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def dependencies():
    for package, expected in (("bellows", "1.0.2"), ("universal-silabs-flasher", "1.1.0")):
        try:
            version = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError as error:
            raise ValueError("Install " + package + "==" + expected + " in the host virtualenv") from error
        if version != expected:
            raise ValueError("Install " + package + "==" + expected + " in the host virtualenv")


def validate_image(path, expected=IMAGE_SHA256):
    from universal_silabs_flasher.firmware import parse_firmware_image
    data = path.read_bytes()
    if digest(data) != expected:
        raise ValueError("Firmware SHA-256 mismatch")
    firmware = parse_firmware_image(data)  # validates complete GBL CRC
    programs = 0
    # Restrict to the unencrypted, uncompressed app-only format we build. Unknown
    # tags cannot silently bypass the bootloader/NVM range checks.
    allowed = {0x03A617EB, 0xF40A0AF4, 0xFE0101FE, 0xFD0303FD,
               0xF60808F6, 0xFA0606FA, 0xFC0404FC}
    for tag, value in firmware.tags:
        if int(tag) not in allowed:
            raise ValueError("Unsupported/non-application GBL tag: " + hex(int(tag)))
        if int(tag) in (0xFE0101FE, 0xFD0303FD):
            if len(value) <= 4:
                raise ValueError("Empty or truncated application program tag")
            address = struct.unpack_from("<I", value)[0]
            if address < 0x4000 or address + len(value) - 4 > 0x37000:
                raise ValueError("Program overlaps reserved bootloader/NVM3 space")
            programs += 1
    if not programs:
        raise ValueError("GBL contains no application program data")
    print("Application-only GBL validated; SHA-256 " + digest(data))
    return firmware


def validate_bridge(data, expected=None):
    if expected and digest(data) != expected:
        raise ValueError("serialgateway SHA-256 mismatch")
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x02":
        raise ValueError("serialgateway must be a big-endian ELF32 executable")
    if struct.unpack_from(">HH", data, 16) != (2, 8):
        raise ValueError("serialgateway must be a MIPS executable")
    phoff = struct.unpack_from(">I", data, 28)[0]
    phsize, phcount = struct.unpack_from(">HH", data, 42)
    if phsize < 32 or phcount < 1 or phoff + phsize * phcount > len(data):
        raise ValueError("Invalid serialgateway ELF program headers")
    for offset in range(phoff, phoff + phsize * phcount, phsize):
        if struct.unpack_from(">I", data, offset)[0] in (2, 3):
            raise ValueError("serialgateway must be statically linked")
    # The pinned upstream build contains these CLI options/OOB support. A local
    # override must have the same reviewed bytes; arbitrary compatible-looking
    # executables are not accepted as a substitute.
    if digest(data) != BRIDGE_SHA256:
        raise ValueError("Unreviewed serialgateway; use the documented Release-1.2 binary")


def bridge_bytes(path=None):
    if path:
        data = path.read_bytes()
    else:
        print("Downloading pinned serialgateway on the host: " + BRIDGE_URL)
        with urllib.request.urlopen(BRIDGE_URL, timeout=30) as response:
            data = response.read(1024 * 1024)
    validate_bridge(data, BRIDGE_SHA256)
    return data


def parse_processes(output):
    result = {}
    for line in output.splitlines():
        pid, state, owner, executable = line.split("\t")
        parent, start, status = state.split()
        if not all(value.isdigit() for value in (pid, parent, start)) or owner not in ("0", "1"):
            raise ValueError("Invalid process inspection result")
        result[int(pid)] = {"parent": int(parent), "start": start,
                            "status": status, "owner": owner == "1", "exe": executable}
    return result


def descended_from(pid, ancestor, processes):
    """Old uClibc LinuxThreads exposes workers as child PIDs, not one TGID."""
    seen = set()
    while pid != ancestor:
        if pid in seen or pid not in processes:
            return False
        seen.add(pid)
        parent = processes[pid]["parent"]
        if parent != ancestor and (parent not in processes or
                                   processes[parent]["exe"] != processes[pid]["exe"]):
            return False
        pid = parent
    return True


def crc16(body):
    fcs = 0xFFFF
    for byte in body:
        fcs ^= byte
        for _ in range(8):
            fcs = (fcs >> 1) ^ (0x8408 if fcs & 1 else 0)
    return fcs ^ 0xFFFF


def hdlc(body):
    body = body + struct.pack("<H", crc16(body))
    escaped = bytearray(b"\x7e")
    for byte in body:
        escaped.extend((0x7D, byte ^ 0x20) if byte in (0x7E, 0x7D, 0x11, 0x13, 0xF8) else (byte,))
    return bytes(escaped + b"\x7e")


def spinel_reset():
    # The active Zigbee instance is IID1; IID0 has no multipan instance.
    return hdlc(b"\x90\x01\x03")


def verify_hdlc(host, port):
    """Require an IID1 NCP_VERSION reply after flashing; do not form a network."""
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.settimeout(2)
        connection.sendall(hdlc(b"\x91\x02\x02"))
        frame = bytearray()
        escaped = False
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                data = connection.recv(1024)
            except socket.timeout:
                continue
            if not data:
                break
            for byte in data:
                if byte == 0x7E:
                    if len(frame) >= 5 and not escaped:
                        body = frame[:-2]
                        if (crc16(body) == struct.unpack("<H", frame[-2:])[0]
                                and body.startswith(b"\x91\x06\x02")
                                and body[3:].endswith(b"\x00") and len(body[3:]) > 1):
                            print("HDLC IID1 NCP_VERSION response verified")
                            return
                    frame.clear()
                    escaped = False
                elif escaped:
                    frame.append(byte ^ 0x20)
                    escaped = False
                elif byte == 0x7D:
                    escaped = True
                else:
                    frame.append(byte)
                if len(frame) > 4096:
                    raise ValueError("Oversized HDLC response")
        raise ValueError("New firmware did not respond to an IID1 Spinel version query")


def menu(host, port):
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.settimeout(2)
        connection.sendall(b"\n")
        data = b""
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline and len(data) < 4096:
            try:
                chunk = connection.recv(1024)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk
            if all(marker in data for marker in (b"Gecko Bootloader", b"1. upload gbl", b"2. run", b"BL >")):
                return
        raise ValueError("No recognized Gecko bootloader menu; upload refused")


async def enter_ezsp(host, port):
    import bellows.ezsp
    import bellows.types as types
    radio = bellows.ezsp.EZSP({"path": f"socket://{host}:{port}", "baudrate": 115200,
                              "flow_control": "hardware"})
    try:
        await radio.connect(use_thread=False)
        status, = await radio.launchStandaloneBootloader(mode=1)
        if status != types.EmberStatus.SUCCESS:
            raise ValueError("EZSP bootloader entry rejected")
    finally:
        await radio.disconnect()


async def upload(host, port, firmware):
    from universal_silabs_flasher.flasher import Flasher
    flasher = Flasher(device=f"socket://{host}:{port}")
    flasher.bootloader_baudrate = 115200
    await asyncio.wait_for(flasher.flash_firmware(firmware, run_firmware=True), 180)


class Gateway:
    def __init__(self, args):
        self.args = args
        self.ssh = ["ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=yes",
                    "-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa",
                    "-p", str(args.ssh_port)]
        if args.identity:
            self.ssh += ["-i", str(args.identity)]
        if args.password_file:
            if not args.password_file.is_file() or not shutil.which("sshpass"):
                raise ValueError("--password-file requires a readable file and host sshpass")
            self.ssh = ["sshpass", "-f", str(args.password_file), *self.ssh,
                        "-o", "PreferredAuthentications=password", "-o", "PubkeyAuthentication=no"]
        self.ssh += ["root@" + args.host]
        self.directory = None
        self.locked = False
        self.bridge = None
        self.bridge_pid = None
        self.bridge_start = None
        self.paired_running = False
        self.success = False

    def remote(self, command, data=None):
        return subprocess.check_output(self.ssh + [command], input=data, timeout=45).decode().strip()

    def processes(self):
        return parse_processes(self.remote("sh -c " + shlex.quote(SCAN)))

    def guard(self, pid, start):
        return ("test \"$(sed 's/.*) //' /proc/" + str(pid) +
                "/stat | awk '{print $20}')\" = " + shlex.quote(start))

    def free(self, expected=None):
        processes = self.processes()
        owners = {pid: process for pid, process in processes.items() if process["owner"]}
        if expected is None:
            if owners:
                raise ValueError("UART still owned by PIDs " + ", ".join(map(str, owners)))
        elif (expected not in owners or owners[expected]["exe"] != self.bridge
              or owners[expected]["start"] != self.bridge_start or
              any(p["exe"] != self.bridge or not descended_from(pid, expected, processes)
                  for pid, p in owners.items())):
            raise ValueError("UART is not owned exclusively by this invocation's bridge")

    def provision(self):
        self.remote("test \"$(awk '/^Uid:/ {print $2}' /proc/self/status)\" = 0 && "
                    "test -c /dev/ttyS1 && grep -qi mips /proc/cpuinfo && "
                    "test -r /proc/net/tcp && md5sum /dev/null >/dev/null && "
                    "ls -lnL /dev/ttyS1 >/dev/null")
        # Read installed bytes rather than executing an unknown binary for --help.
        data = None
        if self.args.serialgateway:
            data = bridge_bytes(self.args.serialgateway)
        else:
            installed = subprocess.run(self.ssh + ["test -x /tuya/serialgateway && cat /tuya/serialgateway"],
                                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
            if installed.returncode == 0 and digest(installed.stdout) == BRIDGE_SHA256:
                validate_bridge(installed.stdout)
                self.bridge = "/tuya/serialgateway"
            else:
                data = bridge_bytes()
        self.remote("umask 077; mkdir /tmp/microhad-radio-release.lock")
        self.locked = True
        directory = "/tmp/microhad-radio-release." + secrets.token_hex(8)
        physical = self.remote("umask 077; mkdir " + shlex.quote(directory) +
                               " && cd " + shlex.quote(directory) + " && pwd -P")
        if physical not in (directory, "/var" + directory):
            raise ValueError("Unexpected physical temporary directory: " + physical)
        self.directory = physical
        if data is None:
            return
        self.bridge = physical + "/serialgateway"
        self.remote("umask 077; cat > " + shlex.quote(self.bridge), data)
        # Old BusyBox may lack SHA-256: the host verified SHA-256 before SSH;
        # MD5 here checks transfer integrity, not download authenticity.
        self.remote("test \"$(md5sum " + shlex.quote(self.bridge) + " | awk '{print $1}')\" = " +
                    hashlib.md5(data).hexdigest() + " && chmod 700 " + shlex.quote(self.bridge))

    def stop_services(self):
        processes = self.processes()
        services = {pid: p for pid, p in processes.items() if p["exe"] in
                    ("/tuya/native/microhad", "/tuya/native/ziggurat", "/tuya/native/mthread-matter")}
        supervisors = {pid: p for pid, p in processes.items() if p["exe"] in
                       ("/tuya/native/paired_start.sh", "/tuya/tuya_start.sh")}
        # Never terminate an existing bridge: it may belong to ZHA, a migration
        # capture or another operator. The README explains releasing it first.
        if any(p["owner"] and pid not in services for pid, p in processes.items()):
            raise ValueError("Unknown/existing bridge UART owner; stop its client and supervisor first")
        if not services and not supervisors:
            return
        if (self.args.radio == "ezsp" and self.args.already_in_bootloader and not services
                and all(p["exe"] == "/tuya/tuya_start.sh" and p.get("status") == "T"
                        for p in supervisors.values())):
            self.remote("grep -q '/tuya/native/microhad --config' /tuya/tuya_start.sh && "
                        "! grep -q paired_start /tuya/tuya_start.sh")
            print("Prepared migration: leaving the suspended EZSP supervisor untouched")
            return
        paired = any(p["exe"] == "/tuya/native/ziggurat" for p in services.values())
        if paired:
            if self.args.radio != "hdlc" or not self.args.frozen_snapshot_saved:
                raise ValueError("Running HDLC stack requires --radio hdlc and --frozen-snapshot-saved; "
                                 "quiesce and save a frozen snapshot using the recovery guide first")
            self.remote("test -f /tmp/ziggurat-supervisor/quiesced && "
                        "test -x /tuya/native/paired_control.sh && "
                        "grep -q paired_start /tuya/tuya_start.sh")
            supervisor = self.remote("cat /tmp/ziggurat-supervisor/supervisor.pid")
            if (not supervisor.isdigit() or set(supervisors) != {int(supervisor)} or
                    any(not descended_from(pid, int(supervisor), processes) for pid in services)):
                raise ValueError("Paired supervisor/children do not match recorded process identities")
            self.remote(self.guard(int(supervisor), supervisors[int(supervisor)]["start"]))
            self.paired_running = True
            self.remote("/tuya/native/paired_control.sh stop --frozen-snapshot-saved")
        else:
            if self.args.radio != "ezsp" or any(p["exe"] != "/tuya/native/microhad" for p in services.values()):
                raise ValueError("Unsupported service layout; perform the documented orderly stop first")
            leaders = {pid: p for pid, p in services.items() if p["parent"] in supervisors}
            if not leaders:
                raise ValueError("No recognized coordinator supervisor/leader process")
            for pid, process in services.items():
                if not any(descended_from(pid, leader, processes) for leader in leaders):
                    raise ValueError("Unrecognized coordinator process ancestry")
            for pid, process in leaders.items():
                parent = supervisors.get(process["parent"])
                if not parent or parent["exe"] != "/tuya/tuya_start.sh":
                    raise ValueError("Unrecognized coordinator supervisor; stop it manually first")
            # Validate every parent before signaling any. Only the known legacy
            # loop is supported; paired and factory scripts are not inferred.
            self.remote("grep -q '/tuya/native/microhad --config' /tuya/tuya_start.sh && "
                        "! grep -q paired_start /tuya/tuya_start.sh")
            for pid, process in supervisors.items():
                if pid not in {p["parent"] for p in leaders.values()}:
                    raise ValueError("Unexpected extra launcher; stop it manually first")
            for pid, process in supervisors.items():
                self.remote(self.guard(pid, process["start"]) + " && kill -TERM " + str(pid))
            for pid, process in leaders.items():
                self.remote("if test -d /proc/" + str(pid) + "; then " +
                            self.guard(pid, process["start"]) + " && kill -TERM " + str(pid) + "; fi")
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            active = self.processes()
            if not any(p["exe"] in ("/tuya/native/microhad", "/tuya/native/ziggurat",
                                      "/tuya/native/mthread-matter", "/tuya/native/paired_start.sh",
                                      "/tuya/tuya_start.sh") for p in active.values()):
                self.free()
                return
            time.sleep(1)
        raise TimeoutError("Recognized services did not stop; no firmware upload attempted")

    def start_bridge(self, baud, flow):
        self.free()
        command = (shlex.quote(self.bridge) + " -d /dev/ttyS1 -p " + str(self.args.port) +
                   " -b " + str(baud) + ("" if flow else " -f"))
        # Refuse a port collision before spawning, then verify that the new
        # process remains the sole UART owner (a failed bind exits upstream).
        port = f"{self.args.port:04X}"
        self.remote("tables=/proc/net/tcp; if test -r /proc/net/tcp6; then tables=\"$tables /proc/net/tcp6\"; fi; "
                    "awk '$4 == \"0A\" && toupper($2) ~ /:" + port +
                    "$/ {found=1} END {exit found ? 1 : 0}' $tables")
        log = shlex.quote(self.directory + "/bridge.log")
        pid = self.remote(command + " </dev/null >" + log + " 2>&1 & echo $!")
        if not pid.isdigit() or int(pid) <= 1:
            raise ValueError("Invalid bridge PID")
        self.bridge_pid = int(pid)
        time.sleep(1)
        process = self.processes().get(self.bridge_pid)
        if not process or process["exe"] != self.bridge:
            raise ValueError("serialgateway failed to start; inspect " + self.directory + "/bridge.log")
        self.bridge_start = process["start"]
        self.free(self.bridge_pid)

    def stop_bridge(self):
        if self.bridge_pid is None:
            return
        pid = self.bridge_pid
        # Revalidate both start time and executable to avoid killing a reused PID.
        process = self.processes().get(pid)
        if process:
            if process["exe"] != self.bridge or process["start"] != self.bridge_start:
                raise ValueError("Bridge PID identity changed; refusing to signal it")
            self.remote(self.guard(pid, self.bridge_start) + " && kill -TERM " + str(pid))
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if pid not in self.processes():
                    break
                time.sleep(.25)
            else:
                raise TimeoutError("Bridge did not stop")
        self.bridge_pid = None
        self.bridge_start = None
        self.free()

    def run(self, firmware):
        self.provision()
        self.stop_services()
        self.free()
        if not self.args.already_in_bootloader:
            self.start_bridge(460800 if self.args.radio == "hdlc" else 115200, True)
            if self.args.radio == "hdlc":
                with socket.create_connection((self.args.host, self.args.port), timeout=5) as connection:
                    connection.sendall(spinel_reset())
                    time.sleep(.5)
            else:
                asyncio.run(asyncio.wait_for(enter_ezsp(self.args.host, self.args.port), 30))
            time.sleep(3)
            self.stop_bridge()
        self.start_bridge(115200, False)
        menu(self.args.host, self.args.port)
        self.free(self.bridge_pid)
        asyncio.run(upload(self.args.host, self.args.port, firmware))
        print("Bootloader accepted the GBL; application run requested")
        self.stop_bridge()
        time.sleep(2)
        self.start_bridge(460800, True)
        verify_hdlc(self.args.host, self.args.port)
        self.stop_bridge()
        if self.args.radio == "hdlc" and self.paired_running:
            self.remote("/tuya/tuya_start.sh </dev/null >/dev/null 2>&1")
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                if any(p["owner"] and p["exe"] == "/tuya/native/ziggurat" for p in self.processes().values()):
                    self.success = True
                    print("Paired stack restarted; verify network status and real device traffic in the dashboard")
                    return
                time.sleep(1)
            raise TimeoutError("Paired stack did not reclaim the radio; inspect gateway logs")
        self.success = True
        print("Firmware uploaded. " + ("EZSP services remain stopped. Migration: " + MIGRATION_URL
                                       if self.args.radio == "ezsp" else
                                       "Previously stopped services remain stopped; start your compatible HDLC stack."))

    def cleanup(self):
        try:
            self.stop_bridge()
            if self.directory:
                self.remote("rm -f " + shlex.quote(self.directory + "/serialgateway"))
                if self.success:
                    self.remote("rm -f " + shlex.quote(self.directory + "/bridge.log") +
                                " && rm -r " + shlex.quote(self.directory))
                else:
                    print("Temporary diagnostics retained under " + self.directory, file=sys.stderr)
            if self.locked:
                self.remote("rm -r /tmp/microhad-radio-release.lock")
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print("Temporary bridge cleanup incomplete: " + str(error), file=sys.stderr)
            raise ValueError("Inspect UART ownership and /tmp/microhad-radio-release.lock before retrying") from error


def host(value):
    # IPv4 or DNS name only: no SSH option injection, user, shell syntax or ports.
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", value):
        raise argparse.ArgumentTypeError("Use an IPv4 address or DNS hostname")
    if re.fullmatch(r"[0-9.]+", value):
        try:
            ipaddress.IPv4Address(value)
        except ValueError as error:
            raise argparse.ArgumentTypeError(str(error)) from error
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=host)
    parser.add_argument("--radio", choices=("hdlc", "ezsp"))
    parser.add_argument("--flash", action="store_true")
    parser.add_argument("--acknowledge-ezsp-replacement", action="store_true")
    parser.add_argument("--frozen-snapshot-saved", action="store_true")
    parser.add_argument("--already-in-bootloader", action="store_true")
    parser.add_argument("--serialgateway", type=Path, help="Offline copy of pinned upstream Release-1.2 binary")
    parser.add_argument("--ssh-port", type=int, default=22)
    parser.add_argument("--identity", type=Path, help="SSH private-key path; otherwise use normal SSH authentication")
    parser.add_argument("--password-file", type=Path, help="SSH password file pathname (requires host sshpass)")
    parser.add_argument("--port", type=int, default=8888, help="Temporary gateway TCP bridge port")
    args = parser.parse_args(argv)
    if not all(1 <= port <= 65535 for port in (args.port, args.ssh_port)):
        parser.error("Ports must be between 1 and 65535")
    if args.flash and (not args.host or not args.radio):
        parser.error("Flashing requires explicit --host and --radio hdlc|ezsp")
    if args.radio == "ezsp":
        print(WARNING, file=sys.stderr, flush=True)
        if args.flash and not args.acknowledge_ezsp_replacement:
            parser.error("Read the warning and pass --acknowledge-ezsp-replacement to continue")
    dependencies()
    firmware = validate_image(Path(__file__).resolve().with_name("ot-rcp-multipan.gbl"))
    if not args.flash:
        print("Validation only; no device connection or download made")
        return
    gateway = Gateway(args)
    try:
        gateway.run(firmware)
    finally:
        failed = sys.exc_info()[0] is not None
        try:
            gateway.cleanup()
        except Exception:
            # Cleanup reports its own diagnostics; do not hide the original
            # failed preflight/upload behind a secondary cleanup exception.
            if not failed:
                raise
        if not gateway.success:
            print("Flashing did not complete. Services have not been automatically restarted. "
                  "Inspect the bootloader/application and UART ownership before retrying. "
                  "Preserve all backups and network state. " + MIGRATION_URL, file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TimeoutError, subprocess.SubprocessError,
            importlib.metadata.PackageNotFoundError) as error:
        sys.exit("Error: " + str(error))
