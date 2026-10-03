#!/usr/bin/env python3
"""Offline Ziggurat protocol-v2 state import and counter-safe rollback preparation.

This CLI never connects to a radio or flashes/restores hardware. It produces
private command bundles for a future native adapter. --simulate uses only a
new host server attached to a loopback RCP simulator.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from network_backup import open_coordinator, private_write

MAX_COUNTER = 0xffffffff
MARGIN = 10000


def validate(state):
    if state.get("format") != "microhad-ziggurat-migration" or state.get("version") != 1 or state.get("eui_encoding") != "little-endian-wire":
        raise ValueError("unsupported migration state")
    n = state["network"]
    for field, length in [("ieee_address",8),("extended_pan_id",8),("network_key",16),("tc_link_key",16)]:
        value = bytes.fromhex(n[field])
        if len(value) != length or value in (bytes(length), bytes([255])*length):
            raise ValueError("missing or invalid identity/key")
    if n.get("tclk_seed") is not None and len(bytes.fromhex(n["tclk_seed"])) != 16:
        raise ValueError("invalid trust-center seed")
    if n["channel"] not in range(11,27) or n["pan_id"] not in range(1,65535) or n["nwk_address"] != 0:
        raise ValueError("invalid coordinator parameters")
    if n["tclk_flavor"] != "ezsp" or not 0 <= n["network_key_seq"] <= 255 or not 0 <= n["nwk_update_id"] <= 255 or not -128 <= n["tx_power"] <= 127:
        raise ValueError("unsupported security/radio settings")
    for field in ("network_key_tx_counter","aps_frame_counter"):
        if not isinstance(n[field],int) or not 0 <= n[field] <= MAX_COUNTER:
            raise ValueError("invalid security counter")
    identities = set()
    for key in state["keys"]:
        if len(bytes.fromhex(key["ieee"])) != 8 or len(bytes.fromhex(key["key"])) != 16 or key["ieee"] in identities:
            raise ValueError("invalid or duplicate device key")
        identities.add(key["ieee"])
        for field in ("tx_counter","rx_counter"):
            if not isinstance(key[field],int) or not 0 <= key[field] <= MAX_COUNTER:
                raise ValueError("invalid device counter")
        # Ziggurat restores key entries as verified. Do not promote unverified keys.
        if key.get("bitmask",0) & 0x10 == 0:
            raise ValueError("device key is not marked authorized; import cannot promote it")
    child_ids = set()
    for child in state["children"]:
        if len(bytes.fromhex(child["ieee"])) != 8 or not 1 <= child["nwk"] <= 0xfff7 or child["ieee"] in child_ids or child["type"] not in (2,3,4,5):
            raise ValueError("unsupported/duplicate child")
        child_ids.add(child["ieee"])
    return state


def advance(value, margin=MARGIN):
    if value > MAX_COUNTER - margin:
        raise ValueError("security counter exhausted; refusing wrap")
    return value + margin


def prepared_state(state, aps_upper_bound=None):
    validate(state)
    prepared = copy.deepcopy(state)
    n = prepared["network"]
    evidence = state.get("counter_evidence",{})
    if aps_upper_bound is None:
        if not evidence.get("aps_live_upper_bound_verified"):
            raise ValueError("APS live counter bound unavailable; require independently verified upper bound")
        aps_upper_bound = n["aps_frame_counter"]
    if not isinstance(aps_upper_bound,int) or not n["aps_frame_counter"] <= aps_upper_bound <= MAX_COUNTER:
        raise ValueError("APS upper bound is below captured state or outside counter range")
    n["network_key_tx_counter"] = advance(n["network_key_tx_counter"])
    n["aps_frame_counter"] = advance(max([aps_upper_bound] + [v["tx_counter"] for v in prepared["keys"]]))
    for key in prepared["keys"]:
        key["tx_counter"] = n["aps_frame_counter"]
        if state.get("incoming_counter_semantics", "last-accepted") == "last-accepted" and key.get("bitmask",0) & 4:
            key["rx_counter"] = advance(key["rx_counter"], 1)
    prepared["incoming_counter_semantics"] = "next-acceptable"
    prepared["counter_preparation"] = {"margin": MARGIN, "requires_fresh_exclusive_handover":True,
                                      "crash_rollback_verified":False}
    return prepared


def encode_network(n):
    return (struct.pack("<BBH", n["channel"],n["nwk_update_id"],n["pan_id"])
            + bytes.fromhex(n["extended_pan_id"]) + struct.pack("<H",n["nwk_address"])
            + bytes.fromhex(n["ieee_address"]) + bytes.fromhex(n["network_key"])
            + struct.pack("<BI", n["network_key_seq"],n["network_key_tx_counter"])
            + bytes.fromhex(n["tc_link_key"]) + bytes([int(n["tclk_seed"] is not None)])
            + bytes.fromhex(n["tclk_seed"] or "00"*16) + struct.pack("<BbI",1,n["tx_power"],n["aps_frame_counter"]))


def import_commands(state):
    validate(state)
    keys = struct.pack("<H",len(state["keys"]))
    for key in state["keys"]:
        keys += (bytes.fromhex(key["key"]) + struct.pack("<IIB",key["tx_counter"],key["rx_counter"],0)
                 + bytes.fromhex(key["ieee"]))
    children = struct.pack("<H",len(state["children"]))
    addresses = struct.pack("<H",len(state["children"]))
    for child in state["children"]:
        device_type = 1 if child["type"] == 2 else 2
        children += bytes.fromhex(child["ieee"]) + struct.pack("<HB",child["nwk"],int(child["rx_on_when_idle"]) | device_type << 1)
        addresses += bytes.fromhex(child["ieee"]) + struct.pack("<H",child["nwk"])
    return [(0x10,b"\x00\x01"+encode_network(state["network"])),(0x11,keys),(0x12,children),(0x13,addresses)]


def native_state(state):
    """Encode already-prepared migration state; native startup adds its own margin."""
    commands = dict(import_commands(state))
    result = {"format": "microhad-ziggurat-native", "version": 1,
              "network": encode_network(state["network"]).hex(),
              "keys": commands[0x11].hex(), "children": commands[0x12].hex(),
              "addresses": commands[0x13].hex()}
    result["checksum"] = hashlib.sha256(b"".join(bytes.fromhex(result[field])
        for field in ("network", "keys", "children", "addresses"))).hexdigest()
    return result


def verify_import(client, state):
    """Load stopped stack, verify exported state and every key/child, never StartNetwork."""
    for command,payload in import_commands(state): client.request(command,payload)
    info = client.request(0x20)
    if len(info) != 85 or info[:82] != encode_network(state["network"]) or info[82:84] != struct.pack("<H",len(state["keys"])) or info[84] != 0:
        raise ValueError("stopped stack state verification failed")
    def scan(command):
        before = len(client.events)
        client.request(command)
        return [frame[3:] for frame in list(client.events)[before:] if frame[0] == command and frame[1] & 3 == 2]
    got_keys = scan(0x21)
    expected_keys = [bytes.fromhex(v["key"]) + struct.pack("<IIB",state["network"]["aps_frame_counter"],v["rx_counter"],0) + bytes.fromhex(v["ieee"]) for v in state["keys"]]
    if sorted(got_keys) != sorted(expected_keys): raise ValueError("device key restoration mismatch")
    got_children = scan(0x22)
    expected_children = [bytes.fromhex(v["ieee"]) + struct.pack("<HB",v["nwk"],int(v["rx_on_when_idle"]) | (1 if v["type"] == 2 else 2) << 1) for v in state["children"]]
    if sorted(got_children) != sorted(expected_children): raise ValueError("child restoration mismatch")
    return {"stopped_state_verified":True,"keys_verified":len(got_keys),"children_verified":len(got_children),"network_started":False}


def freeze_and_export(client, template):
    """Cooperatively drain tracked tasks, then read a stable final snapshot."""
    from datetime import datetime, timezone
    client.request(0x04) # experimental FreezeNetwork, never a radio reset
    info = client.request(0x20)
    if len(info) != 85 or info[84] != 0:
        raise ValueError("stack did not freeze")
    n = {"channel":info[0],"nwk_update_id":info[1],"pan_id":int.from_bytes(info[2:4],"little"),
         "extended_pan_id":info[4:12].hex(),"nwk_address":int.from_bytes(info[12:14],"little"),
         "ieee_address":info[14:22].hex(),"network_key":info[22:38].hex(),"network_key_seq":info[38],
         "network_key_tx_counter":int.from_bytes(info[39:43],"little"),"tc_link_key":info[43:59].hex(),
         "tclk_seed":info[60:76].hex() if info[59] else None,"tclk_flavor":"ezsp" if info[76]==1 else "zstack",
         "tx_power":struct.unpack("b",info[77:78])[0],"aps_frame_counter":int.from_bytes(info[78:82],"little")}
    def scan(command):
        before=len(client.events);client.request(command)
        return [frame[3:] for frame in list(client.events)[before:] if frame[0]==command and frame[1]&3==2]
    keys=[]
    for row in scan(0x21):
        if len(row)!=33:raise ValueError("invalid frozen key row")
        keys.append({"key":row[:16].hex(),"tx_counter":int.from_bytes(row[16:20],"little"),
                     "rx_counter":int.from_bytes(row[20:24],"little"),"ieee":row[25:33].hex(),"bitmask":0x1e})
    children=[]
    for row in scan(0x22):
        if len(row)!=11:raise ValueError("invalid frozen child row")
        rx=bool(row[10]&1);typ=row[10]>>1&3
        children.append({"ieee":row[:8].hex(),"nwk":int.from_bytes(row[8:10],"little"),
                         "rx_on_when_idle":rx,"type":2 if typ==1 else 3 if rx else 4})
    if client.request(0x20)!=info:raise ValueError("snapshot changed after freeze")
    state=copy.deepcopy(template);state["network"]=n;state["keys"]=keys;state["children"]=children
    state["incoming_counter_semantics"]="next-acceptable"
    state["captured_at"]=datetime.now(timezone.utc).isoformat()
    state["counter_evidence"]={"aps_live_upper_bound_verified":True,"source":"frozen-ziggurat-state"}
    state["handover"]={"stack_stopped":True,"counters_observed_after_stop":True,"freeze_opcode":4}
    validate(state)
    return state


def rollback_state(original, last_state, aps_upper_bound=None):
    """Use LAST STOPPED state, never the stale pre-migration counters or device keys."""
    validate(original); validate(last_state)
    handover = last_state.get("handover",{})
    if not handover.get("stack_stopped") or not handover.get("counters_observed_after_stop"):
        raise ValueError("rollback requires a final frozen snapshot, not a running/stale backup")
    for field in ("ieee_address","pan_id","extended_pan_id","network_key","network_key_seq","tc_link_key","tclk_seed","tclk_flavor"):
        if original["network"][field] != last_state["network"][field]:
            raise ValueError("network identity/security changed; rollback needs a separate migration")
    for field in ("network_key_tx_counter","aps_frame_counter"):
        if last_state["network"][field] < original["network"][field]:
            raise ValueError("last-state counter regressed")
    return prepared_state(last_state, aps_upper_bound)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest="command",required=True)
    p=sub.add_parser("prepare-import")
    p.add_argument("--state",required=True);p.add_argument("--output",required=True)
    p.add_argument("--aps-upper-bound",type=int)
    p=sub.add_parser("prepare-native")
    p.add_argument("--state",required=True);p.add_argument("--output",required=True)
    p=sub.add_parser("prepare-rollback")
    p.add_argument("--original",required=True);p.add_argument("--last-stopped-state",required=True)
    p.add_argument("--output",required=True);p.add_argument("--aps-upper-bound",type=int)
    args=parser.parse_args()
    try:
        if args.command=="prepare-import":
            state=prepared_state(json.loads(Path(args.state).read_text()),args.aps_upper_bound)
            bundle={"format":"ziggurat-import-bundle","protocol_version":2,"state":state,
                    "commands":[{"command":cmd,"payload":payload.hex()} for cmd,payload in import_commands(state)],
                    "start_network_included":False,"hardware_verified":False}
        elif args.command=="prepare-native":
            state=json.loads(Path(args.state).read_text())
            if state.get("incoming_counter_semantics")!="next-acceptable" or not state.get("counter_preparation"):
                raise ValueError("prepare-native requires the state from a prepared import bundle")
            bundle=native_state(state)
        else:
            state=rollback_state(json.loads(Path(args.original).read_text()),json.loads(Path(args.last_stopped_state).read_text()),args.aps_upper_bound)
            bundle=open_coordinator(state)
            bundle["metadata"]["microhad_counter_safety"]={"nwk_floor":state["network"]["network_key_tx_counter"],
                "aps_floor":state["network"]["aps_frame_counter"],"source":"last-stopped-state","hardware_verified":False}
        private_write(args.output,(json.dumps(bundle,indent=2)+"\n").encode())
        print("Private offline migration artifact saved; no radio connection made.")
    except (OSError,ValueError,KeyError,TypeError):
        parser.exit(1,"Migration preparation refused: invalid/incomplete state, missing counter evidence, or unsafe output. No radio connection made.\n")


if __name__=="__main__":main()
