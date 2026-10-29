#!/usr/bin/env python3
"""Read-only network export via the running native daemon, never UART/reset/flash.

Dependencies for decoding: bellows==1.0.2 zigpy==2.2.0. Outputs contain keys.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import urllib.parse
import urllib.request


import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, gateway_host, ssh_command
from gateway_api import urlopen


def private_write(path, data):
    path = Path(path)
    if path.parent.stat().st_mode & 0o077:
        raise ValueError("output directory must have mode 0700")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def ssh_args(args):
    return ssh_command(args.host, args.password_file)


def collect(args):
    # No shell interpolation of user paths or sensitive file contents.
    identifier = str(time.time_ns())
    remote = f"/tmp/microhad-network-backup/{identifier}.json"
    output = Path(args.output)
    if output.exists() or output.parent.stat().st_mode & 0o077:
        raise ValueError("output must be new, inside a mode-0700 directory")
    data = urllib.parse.urlencode({"id": identifier}).encode()
    request = urllib.request.Request(f"http://{args.host}/api/network/backup", data=data, method="POST")
    with urlopen(request, timeout=10) as response:
        if not json.load(response).get("ok"):
            raise ValueError("native export request rejected")
    command = ("if test -f " + shlex.quote(remote) + "; then cat " + shlex.quote(remote) +
               "; elif test -f " + shlex.quote(remote + ".partial") + "; then exit 1; else exit 2; fi")
    deadline = time.monotonic() + 125
    while time.monotonic() < deadline:
        result = subprocess.run(ssh_args(args) + [command], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=12)
        if result.returncode == 0:
            raw = json.loads(result.stdout)
            if raw.get("format") != "microhad-ezsp-export" or not raw.get("complete"):
                raise ValueError("invalid or incomplete native export")
            from datetime import datetime, timezone
            raw["collected_at"] = datetime.now(timezone.utc).isoformat()
            private_write(output, (json.dumps(raw, indent=2) + "\n").encode())
            subprocess.run(ssh_args(args) + ["rm -f " + shlex.quote(remote)],
                           check=True, timeout=12, stdout=subprocess.DEVNULL)
            print("Private raw export saved; records:", len(raw["records"]),
                  "sha256:", hashlib.sha256(result.stdout).hexdigest())
            return raw
        if result.returncode == 2:
            raise ValueError("native export failed; inspect redacted logs")
        time.sleep(1)
    raise TimeoutError("native backup not published; inspect redacted service logs")


def decode_record(record):
    from bellows.ezsp.v13.commands import COMMANDS
    command = record["command"]
    candidates = [(name, tx, rx) for name, (cid, tx, rx) in COMMANDS.items() if cid == command]
    if len(candidates) != 1:
        raise ValueError("unknown or ambiguous export command")
    name, tx, rx = candidates[0]
    data = bytes.fromhex(record["response"])
    if name in ("getTokenInfo", "getChildData", "getNetworkParameters", "getCurrentSecurityState") and len(data) == 1 and data[0] != 0:
        import bellows.types as t
        return name, {"status":t.EmberStatus(data[0])}
    if name in ("getApsKeyInfo", "exportLinkKeyByIndex") and len(data) == 4:
        import bellows.types as t
        return name, {"status":t.sl_Status(int.from_bytes(data,"little"))}
    if isinstance(rx, dict):
        decoded = {}
        for field, typ in rx.items():
            decoded[field], data = typ.deserialize(data)
    else:
        result, data = rx.deserialize(data)
        decoded = result.as_dict()
    if data:
        raise ValueError("unexpected trailing response bytes")
    return name, decoded


def inspect(raw):
    if raw.get("format") != "microhad-ezsp-export" or raw.get("version") != 1 or raw.get("ezsp_version") != 13 or not raw.get("complete"):
        raise ValueError("unsupported or incomplete export")
    decoded = [(record, *decode_record(record)) for record in raw["records"]]
    summary = {}
    for record, name, values in decoded:
        status = values.get("status")
        key = name + (":" + str(status) if status is not None else "")
        summary[key] = summary.get(key, 0) + 1
    return decoded, summary


def normalize(raw):
    """Decode the export without using the native network.json as a key source."""
    from datetime import datetime, timezone
    import bellows.types as t
    decoded, _ = inspect(raw)
    def rows(name):
        return [(r, v) for r, n, v in decoded if n == name]
    def success(values):
        if int(values.get("status", 0)) != 0:
            raise ValueError("required radio export command failed")
        return values
    def stable(name, field):
        values = [success(v)[field] for _, v in rows(name)]
        if len(values) < 2 or values[0] != values[-1]:
            raise ValueError("network identity or keys changed during export")
        return values[-1]
    ieee = stable("getEui64", "eui64")
    params = stable("getNetworkParameters", "parameters")
    security = stable("getCurrentSecurityState", "state")
    if any(int(v["nodeType"]) != 1 for _, v in rows("getNetworkParameters")):
        raise ValueError("only coordinator migration is supported")
    if int(security.bitmask) & 3:
        raise ValueError("high-security/distributed networks are unsupported")
    key_info = success(rows("getNetworkKeyInfo")[-1][1])["network_key_info"]
    initial_info = success(rows("getNetworkKeyInfo")[0][1])["network_key_info"]
    if int(key_info.network_key_frame_counter) < int(initial_info.network_key_frame_counter):
        raise ValueError("NWK counter regressed during export")
    if not key_info.network_key_set or key_info.alternate_network_key_set or key_info.network_key_sequence_number != initial_info.network_key_sequence_number:
        raise ValueError("network key rotation is active or changed during export")
    def export_key(kind):
        values = [success(v)["key"] for r, v in rows("exportKey") if bytes.fromhex(r["request"])[0] == kind]
        if len(values) != 2 or values[0] != values[1]:
            raise ValueError("key export missing or inconsistent")
        return values[0].serialize().hex()
    network_key, tc_material = export_key(1), export_key(2)
    hashed = int(security.bitmask) & 0x84 == 0x84
    if int(params.radioChannel) not in range(11, 27) or int(params.panId) in (0, 0xffff):
        raise ValueError("invalid network parameters")
    tokens = []
    for r, v in rows("getTokenData"):
        success(v)
        request = bytes.fromhex(r["request"])
        tokens.append({"id": int.from_bytes(request[:4], "little"),
                       "index": int.from_bytes(request[4:], "little"), "data": bytes(v["value"]).hex()})
    aps_tokens = [bytes.fromhex(v["data"]) for v in tokens if v["id"] == 0x1e123 and v["index"] == 0]
    if len(aps_tokens) > 1 or any(len(v) != 4 for v in aps_tokens):
        raise ValueError("invalid APS persistence counter token")
    aps_persisted = int.from_bytes(aps_tokens[0], "little") if aps_tokens else None
    keys = []
    for _, v in rows("exportLinkKeyByIndex"):
        if int(v["status"]) in (int(t.sl_Status.NOT_FOUND), int(t.sl_Status.INVALID_INDEX)):
            continue
        success(v)
        metadata = v["key_data"]
        keys.append({"ieee": v["eui64"].serialize().hex(), "key": v["plaintext_key"].serialize().hex(),
                     "tx_counter": int(metadata.outgoing_frame_counter),
                     "rx_counter": int(metadata.incoming_frame_counter), "bitmask": int(metadata.bitmask)})
    if len({v["ieee"] for v in keys}) != len(keys):
        raise ValueError("duplicate device key identities")
    children = []
    for _, v in rows("getChildData"):
        if int(v["status"]) == int(t.EmberStatus.NOT_JOINED):
            continue
        if int(v["status"]) == int(t.EmberStatus.NOT_FOUND):
            continue
        success(v)
        child = v["child_data"]
        children.append({"ieee": child.eui64.serialize().hex(), "nwk": int(child.id),
                         "type": int(child.type), "rx_on_when_idle": int(child.type) in (2, 3)})
    aps_metadata = [int(v["key_data"].outgoing_frame_counter) for _, v in rows("getApsKeyInfo")
                    if int(v["status"]) == 0 and int(v["key_data"].bitmask) & 0x02]
    if aps_metadata and (len(aps_metadata) != 2 or aps_metadata[-1] < aps_metadata[0]):
        raise ValueError("APS counter metadata missing or regressed during export")
    aps_floor = max(([aps_persisted] if aps_persisted is not None else [0]) + aps_metadata + [v["tx_counter"] for v in keys])
    # This is a captured floor, NOT a claim that the persisted token is the live counter.
    state = {"channel": int(params.radioChannel), "nwk_update_id": int(params.nwkUpdateId),
             "pan_id": int(params.panId), "extended_pan_id": params.extendedPanId.serialize().hex(),
             "nwk_address": int(rows("getNodeId")[0][1]["nodeId"]), "ieee_address": ieee.serialize().hex(),
             "network_key": network_key, "network_key_seq": int(key_info.network_key_sequence_number),
             "network_key_tx_counter": max(int(key_info.network_key_frame_counter), int(initial_info.network_key_frame_counter)),
             "tc_link_key": b"ZigBeeAlliance09".hex() if hashed else tc_material,
             "tclk_seed": tc_material if hashed else None, "tclk_flavor": "ezsp",
             "tx_power": int(params.radioTxPower) if int(params.radioTxPower) < 128 else int(params.radioTxPower) - 256, "aps_frame_counter": aps_floor}
    boot = rows("getStandaloneBootloaderVersionPlatMicroPhy")[0][1]
    return {"format": "microhad-ziggurat-migration", "version": 1, "eui_encoding": "little-endian-wire",
            "captured_at": raw.get("collected_at"), "converted_at": datetime.now(timezone.utc).isoformat(), "incoming_counter_semantics":"last-accepted", "network": state,
            "keys": keys, "children": children, "tokens": tokens,
            "bootloader_version": int(boot["bootloader_version"]),
            "counter_evidence": {"aps_persisted": aps_persisted, "aps_metadata": aps_metadata,
                                 "aps_live_upper_bound_verified": len(aps_metadata) == 2,
                                 "device_counter_metadata_available": bool(keys),
                                 "token_export_available": all(int(v.get("status",0)) == 0 for _,v in rows("getTokenInfo"))},
            "hardware_migration_verified": False}


def open_coordinator(state):
    """Produce a standard private backup usable by the retained NCP recovery tools."""
    from zigpy.backups import NetworkBackup
    from zigpy.state import NodeInfo, NetworkInfo, Key
    import zigpy.types as t
    from zigpy.zdo.types import LogicalType
    n = state["network"]
    eui = lambda value: t.EUI64.deserialize(bytes.fromhex(value))[0]
    key = lambda value: t.KeyData(bytes.fromhex(value))
    network = NetworkInfo(pan_id=t.PanId(n["pan_id"]), extended_pan_id=eui(n["extended_pan_id"]),
                          channel=n["channel"], channel_mask=t.Channels.from_channel_list([n["channel"]]),
                          nwk_update_id=n["nwk_update_id"], nwk_manager_id=t.NWK(0), security_level=5,
                          network_key=Key(key=key(n["network_key"]), seq=n["network_key_seq"], tx_counter=n["network_key_tx_counter"]),
                          tc_link_key=Key(key=key(n["tc_link_key"]), tx_counter=n["aps_frame_counter"], partner_ieee=eui(n["ieee_address"])),
                          key_table=[Key(key=key(v["key"]), tx_counter=v["tx_counter"], rx_counter=max(0,v["rx_counter"] - (1 if state.get("incoming_counter_semantics") == "next-acceptable" else 0)), partner_ieee=eui(v["ieee"])) for v in state["keys"]],
                          children=[eui(v["ieee"]) for v in state["children"]],
                          nwk_addresses={eui(v["ieee"]):t.NWK(v["nwk"]) for v in state["children"]},
                          stack_specific={"ezsp":{"hashed_tclk":n["tclk_seed"]}} if n["tclk_seed"] else {},
                          tx_power=n["tx_power"])
    backup = NetworkBackup(node_info=NodeInfo(nwk=t.NWK(n["nwk_address"]), ieee=eui(n["ieee_address"]), logical_type=LogicalType.Coordinator), network_info=network)
    if not backup.is_complete(): raise ValueError("converted backup is incomplete")
    return backup.as_open_coordinator_json()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("collect")
    p.add_argument("--host", default=setting("GATEWAY_IP"), type=gateway_host)
    p.add_argument("--password-file")
    p.add_argument("--output", required=True)
    p = sub.add_parser("inspect")
    p.add_argument("--input", required=True)
    p = sub.add_parser("convert")
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--coordinator-output", required=True)
    args = parser.parse_args()
    try:
        if args.command == "collect": collect(args)
        elif args.command == "convert":
            state = normalize(json.loads(Path(args.input).read_text()))
            coordinator = open_coordinator(state)
            private_write(args.output, (json.dumps(state, indent=2) + "\n").encode())
            private_write(args.coordinator_output, (json.dumps(coordinator, indent=2) + "\n").encode())
            print("Private migration state and coordinator backup saved; keys:", len(state["keys"]), "children:", len(state["children"]), "tokens:", len(state["tokens"]))
        else:
            _, summary = inspect(json.loads(Path(args.input).read_text()))
            print(json.dumps(summary, indent=2)) # statuses and counts only, never keys
    except (OSError, ValueError, TimeoutError, subprocess.SubprocessError):
        parser.exit(1, "Network backup failed; no radio state was written. Check private files and redacted logs.\n")


if __name__ == "__main__":
    main()
