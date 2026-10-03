#!/usr/bin/env python3
"""Synthetic-only tests; never read production keys or connect to hardware."""
import copy
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

import network_backup as backup
import ziggurat_migration as migration


def fixture():
    return {"format":"microhad-ziggurat-migration","version":1,"eui_encoding":"little-endian-wire",
            "network":{"channel":15,"nwk_update_id":3,"pan_id":0x1a62,
                       "extended_pan_id":"0200000000000062","nwk_address":0,
                       "ieee_address":"0200000000000042","network_key":bytes(range(16)).hex(),
                       "network_key_seq":7,"network_key_tx_counter":12000,
                       "tc_link_key":b"ZigBeeAlliance09".hex(),"tclk_seed":bytes(range(16,32)).hex(),
                       "tclk_flavor":"ezsp","tx_power":8,"aps_frame_counter":14000},
            "keys":[{"ieee":"0200000000000011","key":bytes(range(32,48)).hex(),"tx_counter":13900,"rx_counter":42,"bitmask":0x1e}],
            "children":[{"ieee":"0200000000000011","nwk":0x1234,"type":4,"rx_on_when_idle":False}],
            "tokens":[],"counter_evidence":{"aps_live_upper_bound_verified":True},"hardware_migration_verified":False}


def raw_fixture():
    import bellows.types as t
    from bellows.ezsp.v13.commands import COMMANDS
    state=fixture(); n=state["network"]
    eui=lambda v:t.EUI64.deserialize(bytes.fromhex(v))[0]
    params=t.EmberNetworkParameters(extendedPanId=eui(n["extended_pan_id"]),panId=n["pan_id"],radioTxPower=8,radioChannel=15,joinMethod=0,nwkManagerId=0,nwkUpdateId=3,channels=t.Channels.from_channel_list([15]))
    sec=t.EmberCurrentSecurityState(bitmask=0x84,trustCenterLongAddress=eui(n["ieee_address"]))
    info=t.SecurityManagerNetworkKeyInfo(network_key_set=1,alternate_network_key_set=0,network_key_sequence_number=7,alt_network_key_sequence_number=0,network_key_frame_counter=12000)
    meta=t.SecurityManagerAPSKeyMetadata(bitmask=0x1e,outgoing_frame_counter=14000,incoming_frame_counter=42,ttl_in_seconds=0)
    child=t.EmberChildDataV10(eui64=eui(state["children"][0]["ieee"]),type=4,id=0x1234,phy=0,power=0,timeout=0,timeout_remaining=1000)
    raw={"format":"microhad-ezsp-export","version":1,"ezsp_version":13,"complete":True,"records":[]}
    def add(name,data,request=b""):
        raw["records"].append({"command":COMMANDS[name][0],"request":request.hex(),"response":data.hex()})
    for _ in range(2):
        add("getEui64",eui(n["ieee_address"]).serialize());add("getNetworkParameters",b"\x00\x01"+params.serialize())
        add("getCurrentSecurityState",b"\x00"+sec.serialize());add("getNetworkKeyInfo",bytes(4)+info.serialize())
        add("exportKey",bytes.fromhex(n["network_key"])+bytes(4),b"\x01"+bytes(17))
        add("exportKey",bytes.fromhex(n["tclk_seed"])+bytes(4),b"\x02"+bytes(17))
    add("getNodeId",bytes(2));add("getStandaloneBootloaderVersionPlatMicroPhy",struct.pack("<HBBB",0x180,4,23,0))
    add("exportLinkKeyByIndex",eui(state["keys"][0]["ieee"]).serialize()+bytes.fromhex(state["keys"][0]["key"])+meta.serialize()+bytes(4),b"\x00")
    add("getChildData",b"\x00"+child.serialize(),b"\x00")
    for _ in range(2):
        add("getApsKeyInfo",eui(n["ieee_address"]).serialize()+meta.serialize()+bytes(4),b"\x02"+bytes(17))
    add("getTokenInfo",b"\xb5",b"\x00")
    return raw


class MigrationTests(unittest.TestCase):
    def test_decode_missing_token_library_and_identity(self):
        state=backup.normalize(raw_fixture())
        self.assertEqual(state["network"]["ieee_address"],fixture()["network"]["ieee_address"])
        self.assertEqual(state["network"]["tclk_seed"],fixture()["network"]["tclk_seed"])
        self.assertFalse(state["counter_evidence"]["token_export_available"])
        self.assertEqual(state["network"]["aps_frame_counter"],14000)
        self.assertTrue(state["counter_evidence"]["aps_live_upper_bound_verified"])
    def test_absent_aps_metadata_refuses_import(self):
        raw=raw_fixture()
        raw["records"]=[r for r in raw["records"] if r["command"]!=0x10c]
        state=backup.normalize(raw)
        with self.assertRaises(ValueError):migration.prepared_state(state)
    def test_changed_identity_rejected(self):
        raw=raw_fixture(); rows=[r for r in raw["records"] if r["command"]==0x26];rows[-1]["response"]="0300000000000042"
        with self.assertRaises(ValueError):backup.normalize(raw)
    def test_wire_layout_and_endianness(self):
        prepared=migration.prepared_state(fixture());cmds=migration.import_commands(prepared)
        self.assertEqual(len(cmds[0][1]),84);self.assertEqual(cmds[0][1][16:24],bytes.fromhex(fixture()["network"]["ieee_address"]))
        self.assertEqual([c for c,_ in cmds],[0x10,0x11,0x12,0x13])
        self.assertEqual(struct.unpack_from("<I",cmds[0][1],41)[0],22000)
    def test_native_state_conversion_preserves_prepared_counters(self):
        import hashlib
        state=migration.prepared_state(fixture());native=migration.native_state(state)
        wire=bytes.fromhex(native["network"])
        self.assertEqual(int.from_bytes(wire[39:43],"little"),state["network"]["network_key_tx_counter"])
        self.assertEqual(int.from_bytes(wire[78:82],"little"),state["network"]["aps_frame_counter"])
        self.assertEqual(native["checksum"],hashlib.sha256(b"".join(bytes.fromhex(native[field]) for field in
            ("network","keys","children","addresses"))).hexdigest())

    def test_native_cli_accepts_prepared_bundle_and_private_output(self):
        import os
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/"bundle.json";destination=Path(tmp)/"native.json"
            source.write_text(json.dumps({"format":"ziggurat-import-bundle","state":migration.prepared_state(fixture())}))
            command=[str(Path(__import__("sys").executable)),str(Path(migration.__file__)),"prepare-native","--state",str(source),"--output",str(destination)]
            subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
            self.assertEqual(destination.stat().st_mode&0o777,0o600)
            result=json.loads(destination.read_text());self.assertEqual(result["format"],"microhad-ziggurat-native")
            self.assertNotEqual(subprocess.run(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode,0)

    def test_no_missing_counter_guess(self):
        state=fixture();state["counter_evidence"]["aps_live_upper_bound_verified"]=False
        with self.assertRaises(ValueError):migration.prepared_state(state)
        self.assertEqual(migration.prepared_state(state,15000)["network"]["aps_frame_counter"],25000)
        with self.assertRaises(ValueError):migration.prepared_state(state,13999)
    def test_counter_exhaustion(self):
        state=fixture();state["network"]["network_key_tx_counter"]=0xffffffff-9999
        with self.assertRaises(ValueError):migration.prepared_state(state)
    def test_unverified_device_key_not_promoted(self):
        state=fixture();state["keys"][0]["bitmask"]=0x0e
        with self.assertRaises(ValueError):migration.import_commands(state)
    def test_rollback_uses_last_state_and_both_counters(self):
        original=fixture();last=copy.deepcopy(original);last["network"]["network_key_tx_counter"]=22001;last["network"]["aps_frame_counter"]=25000
        last["keys"][0]["tx_counter"]=25000
        last["handover"]={"stack_stopped":True,"counters_observed_after_stop":True}
        prepared=migration.rollback_state(original,last)
        self.assertEqual(prepared["network"]["network_key_tx_counter"],32001)
        self.assertEqual(prepared["network"]["aps_frame_counter"],35000)
        coordinator=backup.open_coordinator(prepared)
        from zigpy.backups import NetworkBackup
        reloaded=NetworkBackup.from_dict(coordinator)
        self.assertEqual(reloaded.network_info.tc_link_key.tx_counter,35000)
        self.assertTrue(reloaded.is_complete())
    def test_rollback_refuses_running_backup(self):
        with self.assertRaises(ValueError):migration.rollback_state(fixture(),fixture())
    def test_rollback_wrong_identity_and_regressed_counter(self):
        original=fixture();last=copy.deepcopy(original);last["handover"]={"stack_stopped":True,"counters_observed_after_stop":True};last["network"]["ieee_address"]="0300000000000042"
        with self.assertRaises(ValueError):migration.rollback_state(original,last)
        last=copy.deepcopy(original);last["handover"]={"stack_stopped":True,"counters_observed_after_stop":True};last["network"]["aps_frame_counter"]-=1
        with self.assertRaises(ValueError):migration.rollback_state(original,last)
    def test_ncp_restore_advances_both_counter_spaces(self):
        from zigpy.backups import NetworkBackup
        import ezsp_restore
        original=NetworkBackup.from_dict(backup.open_coordinator(fixture()))
        prepared=ezsp_restore.prepared_restore_backup(original)
        self.assertEqual(prepared.network_info.network_key.tx_counter,12000)
        self.assertEqual(prepared.network_info.tc_link_key.tx_counter,24000)
        self.assertEqual(original.network_info.tc_link_key.tx_counter,14000)
        original.network_info.tc_link_key.tx_counter=0xffffffff-9999
        with self.assertRaises(ValueError):ezsp_restore.prepared_restore_backup(original)
    def test_ncp_restore_mock_verifies_metadata_and_no_identity_rewrite(self):
        import asyncio
        from types import SimpleNamespace
        from unittest.mock import patch
        from zigpy.backups import NetworkBackup
        import bellows.types as t
        import ezsp_restore
        original=NetworkBackup.from_dict(backup.open_coordinator(fixture()))
        class FakeApp:
            def __init__(self,config):
                self._ezsp=self;self.backups=self;self.ezsp_version=13;self.writes=0;self.closed=False
            async def connect(self):pass
            async def getEui64(self):return (original.node_info.ieee,)
            async def getApsKeyInfo(self,context_in):
                self.assert_context=int(context_in.core_key_type)==2
                return original.node_info.ieee,SimpleNamespace(bitmask=2,outgoing_frame_counter=14000+self.writes*10000),t.sl_Status.OK
            async def restore_backup(self,value,counter_increment,create_new):
                self.writes+=1;self.actual=copy.deepcopy(value)
                self.actual.network_info.network_key.tx_counter+=counter_increment
            async def create_backup(self,load_devices):return self.actual
            async def shutdown(self):self.closed=True
        app=FakeApp({})
        with tempfile.TemporaryDirectory() as directory:
            args=SimpleNamespace(host="unused",port=0,verify_only=False,output=str(Path(directory)/"verified.json"))
            with patch.object(ezsp_restore,"ControllerApplication",return_value=app):asyncio.run(ezsp_restore.restore(args,original))
            self.assertTrue(app.closed);self.assertEqual(app.writes,1);self.assertTrue(app.assert_context)
            found=NetworkBackup.from_dict(json.loads(Path(args.output).read_text()))
            self.assertEqual(found.network_info.tc_link_key.tx_counter,24000)
            self.assertEqual(found.network_info.network_key.tx_counter,22000)
    def test_private_files_refuse_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"state.json";backup.private_write(path,b"synthetic")
            self.assertEqual(path.stat().st_mode & 0o777,0o600)
            with self.assertRaises(FileExistsError):backup.private_write(path,b"changed")


if __name__=="__main__":unittest.main()
