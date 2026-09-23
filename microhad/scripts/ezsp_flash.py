#!/usr/bin/env python3
"""Application-only Lidl UART upgrade. Defaults to offline validation.

Requires the native daemon AND supervisor stopped, the original serialgateway
bridge running, and a private network backup. Never updates the bootloader.
Uses bellows 1.0.2 and universal-silabs-flasher 1.1.0.
"""
import argparse
import asyncio
import hashlib
import logging
from pathlib import Path
import socket
import time

from ezsp_preflight import inspect_image, inspect_radio


def flow_control(host, port, enabled):
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.send(b"\x11" if enabled else b"\x10", socket.MSG_OOB)
        time.sleep(0.5)  # serialgateway reopens the UART


async def upload(args, firmware):
    from universal_silabs_flasher.flasher import Flasher

    if not args.already_in_bootloader:
        await asyncio.wait_for(inspect_radio(args.host, args.port, launch=True), 30)
        await asyncio.sleep(3) # allow the old Gecko bootloader to start its UART
    flow_control(args.host, args.port, False)
    try:
        flasher = Flasher(device=f"socket://{args.host}:{args.port}")
        flasher.bootloader_baudrate = 115200
        last = [-1]

        def progress(current, total):
            percent = current * 100 // total
            if percent // 10 != last[0]:
                last[0] = percent // 10
                print(f"Upload {percent}%", flush=True)

        # Library requires the bootloader's successful upload response before
        # selecting run. Never retry the entire upload or flash another image.
        await asyncio.wait_for(flasher.flash_firmware(
            firmware, run_firmware=True, progress_callback=progress), 180)
        print("Bootloader accepted image and application run requested", flush=True)
    finally:
        flow_control(args.host, args.port, True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--host")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--flash", action="store_true")
    parser.add_argument("--exclusive-radio", action="store_true")
    parser.add_argument("--already-in-bootloader", action="store_true")
    args = parser.parse_args()
    data = Path(args.image).read_bytes()
    if hashlib.sha256(data).hexdigest() != args.sha256.lower():
        parser.error("Firmware SHA256 mismatch")
    inspect_image(args.image)
    if not args.flash:
        print("Validation only; no radio connection made")
        return
    if not args.host or not args.exclusive_radio:
        parser.error("Flashing requires --host and --exclusive-radio")
    from universal_silabs_flasher.firmware import parse_firmware_image
    logging.basicConfig(level=logging.WARNING)
    asyncio.run(upload(args, parse_firmware_image(data)))


if __name__ == "__main__":
    main()
