#!/usr/bin/env python3
"""Offline audit of the retained NCP recovery image and compiled RCP storage.

Never connects to the gateway, enters its bootloader or writes any firmware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

RECOVERY_SHA256="05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d"


def inspect(image_path, map_path):
    from universal_silabs_flasher.firmware import parse_firmware_image
    data=Path(image_path).read_bytes()
    if hashlib.sha256(data).hexdigest()!=RECOVERY_SHA256:
        raise ValueError("image is not the retained successfully installed NCP 7.5.1 image")
    firmware=parse_firmware_image(data)
    ranges=[]
    for tag,value in firmware.tags:
        if int(tag) in (0xF50909F5,0x5EA617EB):raise ValueError("bootloader/secure-element update forbidden")
        if int(tag) in (0xFD0505FD,0xFD0707FD):raise ValueError("compressed program ranges are not audited")
        if int(tag) in (0xFE0101FE,0xFD0303FD):
            start=struct.unpack_from("<I",value)[0];end=start+len(value)-4
            if start<0x4000 or end>0x37000:raise ValueError("image overlaps bootloader or NVM storage")
            ranges.append([hex(start),hex(end)])
    if not ranges:raise ValueError("no application program ranges")
    text=Path(map_path).read_text()
    def symbol(name):
        match=re.search(r"0x([0-9a-fA-F]+)\s+"+re.escape(name)+r"\s*=",text)
        if not match:raise ValueError("required linker storage symbol missing")
        return int(match[1],16)
    nvm_begin,nvm_end,nvm_size=symbol("linker_nvm_begin"),symbol("linker_nvm_end"),symbol("linker_nvm_size")
    if (nvm_begin,nvm_end,nvm_size)!=(0x37000,0x40000,36864):raise ValueError("RCP NVM layout differs from retained NCP project")
    strong=bool(re.search(r"\.text\.bootloader_rebootAndInstall\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+[^\n]*platform/bootloader/api/btl_interface.o",text))
    if not strong or "otPlatResetToBootloader" not in text:raise ValueError("real RCP bootloader entry implementation missing")
    return {"ncp_version":"7.5.1.0 / EZSP v13","ncp_image_sha256":RECOVERY_SHA256,
            "ncp_image_bytes":len(data),"image_crc_valid":True,"application_ranges":ranges,
            "bootloader_update_included":False,"rcp_nvm_begin":hex(nvm_begin),"rcp_nvm_end":hex(nvm_end),
            "rcp_nvm_size":nvm_size,"rcp_strong_bootloader_entry_linked":strong,
            "physical_radio_accessed":False,"firmware_flashed":False,"hardware_rollback_verified":False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ncp-image",required=True);parser.add_argument("--rcp-map",required=True)
    args=parser.parse_args()
    try:print(json.dumps(inspect(args.ncp_image,args.rcp_map),indent=2))
    except (ValueError,OSError):parser.exit(1,"Recovery inspection failed; no hardware action was performed.\n")


if __name__=="__main__":main()
