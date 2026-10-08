#!/usr/bin/env python3
"""Restore a private Zigbee backup to the SAME radio, never rewrite its IEEE.

Requires exclusive radio access through serialgateway. Host dependencies:
bellows==1.0.2 zigpy==2.2.0. Defaults to offline validation; --restore writes
network state and requires --output for a verified post-restore backup.
"""
import argparse
import copy
import asyncio
import json
import logging
import os
from pathlib import Path

from bellows.zigbee.application import ControllerApplication
from zigpy.backups import NetworkBackup
import bellows.types as t

COUNTER_INCREMENT = 10000

def prepared_restore_backup(backup):
    prepared = copy.deepcopy(backup)
    nwk = prepared.network_info.network_key.tx_counter
    aps = max([prepared.network_info.tc_link_key.tx_counter] +
              [key.tx_counter for key in prepared.network_info.key_table])
    if aps == 0:
        raise ValueError("APS counter evidence is missing; obtain a fresh native export")
    if nwk > 0xFFFFFFFF - COUNTER_INCREMENT or aps > 0xFFFFFFFF - COUNTER_INCREMENT:
        raise ValueError("Security counters cannot safely be advanced")
    prepared.network_info.tc_link_key.tx_counter = aps + COUNTER_INCREMENT
    for key in prepared.network_info.key_table:
        key.tx_counter = aps + COUNTER_INCREMENT
    return prepared

async def read_aps_counter(ezsp):
    if ezsp.ezsp_version != 13:
        raise ValueError("Counter-verified restore currently requires EZSP v13")
    context = t.SecurityManagerContextV13(core_key_type=t.SecurityManagerKeyType.TC_LINK,
        key_index=0, derived_type=0, eui64=t.EUI64([0]*8), multi_network_index=0, flags=0,
        psa_key_alg_permission=0)
    _, metadata, status = await ezsp.getApsKeyInfo(context_in=context)
    if int(status) != 0 or not int(metadata.bitmask) & 0x02:
        raise ValueError("APS counter verification is unavailable")
    return int(metadata.outgoing_frame_counter)



async def restore(args, backup):
    prepared = prepared_restore_backup(backup)
    app = ControllerApplication({
        "device": {"path": f"socket://{args.host}:{args.port}", "baudrate": 115200},
        "backup_enabled": False, "startup_energy_scan": False,
        "database_path": None, "use_thread": False,
    })
    try:
        await app.connect()
        ieee, = await app._ezsp.getEui64()
        if ieee != backup.node_info.ieee:
            raise ValueError("Coordinator IEEE differs; refusing identity rewrite")
        if app._ezsp.ezsp_version != 13:
            raise ValueError("Only the retained EZSP v13 recovery firmware is verified by this tool")
        # Check read capability before any restoration writes.
        await read_aps_counter(app._ezsp)
        if not args.verify_only:
            print("Coordinator identity matches; restoring with both NWK and APS counters advanced", flush=True)
            await app.backups.restore_backup(prepared, counter_increment=COUNTER_INCREMENT, create_new=False)
        actual = await app.backups.create_backup(load_devices=True)
        expected = backup.network_info
        found = actual.network_info
        actual_aps_counter = await read_aps_counter(app._ezsp)
        found.tc_link_key.tx_counter = actual_aps_counter
        checks = {
            "ieee": actual.node_info.ieee == backup.node_info.ieee,
            "pan": found.pan_id == expected.pan_id,
            "extended_pan": found.extended_pan_id == expected.extended_pan_id,
            "channel": found.channel == expected.channel,
            "network_key": found.network_key.key == expected.network_key.key,
            "sequence": found.network_key.seq == expected.network_key.seq,
            "counter": found.network_key.tx_counter >= expected.network_key.tx_counter + COUNTER_INCREMENT,
            "aps_counter": actual_aps_counter >= prepared.network_info.tc_link_key.tx_counter,
            "device_keys": {key.partner_ieee:key.key for key in found.key_table} ==
                           {key.partner_ieee:key.key for key in expected.key_table},
            "trust_center_secret": found.stack_specific.get("ezsp", {}).get("hashed_tclk") ==
                                   expected.stack_specific.get("ezsp", {}).get("hashed_tclk"),
        }
        if not all(checks.values()):
            raise ValueError("Verification failed for fields: " + ", ".join(k for k,v in checks.items() if not v))
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as output:
            json.dump(actual.as_open_coordinator_json(), output, indent=2)
            output.write("\n")
        print("Network identity, keys and advanced NWK/APS counters verified; backup saved", flush=True)
    finally:
        await app.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup", required=True)
    parser.add_argument("--host")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--output")
    parser.add_argument("--restore", action="store_true")
    parser.add_argument("--verify-only", action="store_true",
                        help="Read and verify previously restored state without restoring again")
    parser.add_argument("--exclusive-radio", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.ERROR)
    backup = NetworkBackup.from_dict(json.loads(Path(args.backup).read_text()))
    if not backup.is_complete() or not backup.network_info.stack_specific.get("ezsp", {}).get("hashed_tclk"):
        parser.error("Backup lacks required network identity or hashed trust-center secret")
    try:
        prepared_restore_backup(backup)
    except ValueError as error:
        parser.error(str(error))
    print("Backup validated without printing keys")
    if args.restore and args.verify_only:
        parser.error("Choose restore or verify-only, not both")
    if not args.restore and not args.verify_only:
        return
    if not args.host or not args.exclusive_radio or not args.output:
        parser.error("Restore requires --host, --exclusive-radio and --output")
    if Path(args.output).exists() or not Path(args.output).parent.is_dir():
        parser.error("Output must be a new file in an existing private directory")
    asyncio.run(asyncio.wait_for(restore(args, backup), 90))


if __name__ == "__main__":
    main()
