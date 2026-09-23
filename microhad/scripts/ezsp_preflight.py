#!/usr/bin/env python3
"""Inspect firmware or an exclusively owned EZSP radio; never flash firmware.

Host dependencies: bellows==1.0.2 universal-silabs-flasher==1.1.0.
Radio operations reset the NCP connection. Stop its coordinator and supervisor
first. The socket bridge must implement Paul Banks' flow-control OOB commands.
"""
import argparse
import asyncio
import hashlib
import logging
from pathlib import Path
import socket
import struct
import time


def inspect_image(path):
    from universal_silabs_flasher.firmware import parse_firmware_image

    data = Path(path).read_bytes()
    firmware = parse_firmware_image(data)  # includes whole-image CRC validation
    print(Path(path).name, "bytes=", len(data), "sha256=", hashlib.sha256(data).hexdigest())
    programs = 0
    for tag, value in firmware.tags:
        if int(tag) in (0xF50909F5, 0x5EA617EB):
            raise ValueError("Image contains a bootloader or secure-element update")
        if int(tag) in (0xFE0101FE, 0xFD0303FD):
            programs += 1
            address = struct.unpack_from("<I", value)[0]
            end = address + len(value) - 4
            if address < 0x4000 or end > 0x37000:
                raise ValueError("Program range overlaps reserved Lidl bootloader/NVM3 space")
            print(tag.name, hex(address), "through", hex(end), "(exclusive)")
        elif int(tag) in (0xFD0505FD, 0xFD0707FD):
            raise ValueError("Compressed program ranges need separate validation")
    if not programs:
        raise ValueError("No inspectable application program data")
    print("Image CRC and application ranges valid; storage migration is NOT verified")


async def inspect_radio(host, port, launch):
    import bellows.ezsp
    import bellows.types as t

    ezsp = bellows.ezsp.EZSP({"path": f"socket://{host}:{port}",
                            "baudrate": 115200, "flow_control": "hardware"})
    try:
        await ezsp.connect(use_thread=False)
        print("EZSP:", ezsp.ezsp_version)
        print("Board:", await ezsp.get_board_info())
        print("Bootloader:", await ezsp.getStandaloneBootloaderVersionPlatMicroPhy())
        for name in ("CONFIG_BROADCAST_TABLE_SIZE", "CONFIG_PACKET_BUFFER_COUNT"):
            print(name, await ezsp.getConfigurationValue(getattr(t.EzspConfigId, name)))
        if launch:
            status, = await ezsp.launchStandaloneBootloader(mode=1)
            if status != t.EmberStatus.SUCCESS:
                raise RuntimeError("Bootloader entry rejected")
    finally:
        await ezsp.disconnect()


def return_from_bootloader(host, port):
    # No upload command exists in this script. Only request the menu and run.
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(5)
        sock.send(b"\x10", socket.MSG_OOB)
        time.sleep(0.5)  # switching flow control reopens the bridge's UART
        try:
            sock.sendall(b"\n")
            data = b""
            deadline = time.monotonic() + 10
            while b"BL >" not in data:
                if time.monotonic() >= deadline or len(data) > 4096:
                    raise RuntimeError("No recognizable bootloader menu")
                chunk = sock.recv(1024)
                if not chunk:
                    raise RuntimeError("Bridge disconnected")
                data += chunk
            if b"Gecko Bootloader" not in data or b"2. run" not in data:
                raise RuntimeError("Unexpected menu; run command not sent")
            print(data.decode("ascii", errors="replace"))
            sock.sendall(b"2\n")
            time.sleep(1)
            print("Requested existing application; verify EZSP before resuming daemon")
        finally:
            sock.send(b"\x11", socket.MSG_OOB)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    image = sub.add_parser("image")
    image.add_argument("paths", nargs="+")
    for name in ("radio", "bootloader-roundtrip", "bootloader-return"):
        command = sub.add_parser(name)
        command.add_argument("--host", required=True)
        command.add_argument("--port", type=int, default=8888)
        command.add_argument("--exclusive-radio", action="store_true", required=True,
                             help="Confirm native daemon and supervisor are stopped")
    args = parser.parse_args()
    logging.basicConfig(level=logging.ERROR)  # never log EZSP key payloads
    if args.command == "image":
        for path in args.paths:
            inspect_image(path)
        return
    if args.command != "bootloader-return":
        asyncio.run(asyncio.wait_for(inspect_radio(
            args.host, args.port, args.command == "bootloader-roundtrip"), 30))
    if args.command != "radio":
        return_from_bootloader(args.host, args.port)


if __name__ == "__main__":
    main()
