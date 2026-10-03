#!/usr/bin/env python3
"""Synthetic deployment-tool checks; no gateway access or production secrets."""
import asyncio
import copy
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

from test_network_migration import fixture, raw_fixture
from network_backup import normalize
from ezsp_final_capture import capture, compare, raw_read_protocol
from ziggurat_migration import freeze_and_export, encode_network
from ziggurat_cobs import encode

ROOT=Path(__file__).resolve().parents[1]


class Tools(unittest.TestCase):
    def test_final_capture_raw_error_rows_and_read_only_commands(self):
        from bellows.ezsp.v13.commands import COMMANDS
        source=raw_fixture()
        class Peer:
            def __getattr__(self,name):
                async def reply(*args):
                    cid=COMMANDS[name][0]
                    payload=b''.join(v.serialize() for v in args)
                    candidates=[r for r in source['records'] if r['command']==cid and r['request']==payload.hex()]
                    if candidates:return bytes.fromhex(candidates[0]['response'])
                    if name=='getConfigurationValue':return b'\x00\x01\x00'
                    if name=='getTokenCount':return b'\x02'
                    raise AssertionError('Unexpected command '+name)
                return reply
        raw=asyncio.run(capture(Peer()));compare(normalize(source),normalize(raw))
        self.assertTrue(raw['complete'])
        self.assertFalse(any(r['command'] in (0x1,0x14,0x41,0x44,0x9f) for r in raw['records']))
        self.assertEqual(sum(r['command']==0x101 for r in raw['records']),1)

    def test_raw_adapter_is_instance_local_and_handles_short_reply(self):
        from bellows.ezsp.v13.commands import COMMANDS
        class Protocol:
            pass
        class Peer:
            pass
        p=Peer();p._protocol=Protocol();p._protocol.COMMANDS=COMMANDS
        p._protocol.COMMANDS_BY_ID={cid:(n,tx,rx) for n,(cid,tx,rx) in COMMANDS.items()}
        raw_read_protocol(p)
        rx=p._protocol.COMMANDS['getChildData'][2]
        self.assertEqual(rx.deserialize(b'\x93'),(b'\x93',b''))
        self.assertIsInstance(COMMANDS['getChildData'][2],dict)

    def test_final_comparison_rejects_identity_and_counter_regression(self):
        a=fixture();b=copy.deepcopy(a);b['network']['aps_frame_counter']-=1
        with self.assertRaises(ValueError):compare(a,b)
        b=copy.deepcopy(a);b['network']['network_key']='ff'*16
        with self.assertRaises(ValueError):compare(a,b)

    def test_freeze_rejects_partial_scan(self):
        class Peer:
            events=[]
            def request(self,command):
                if command==4:return b''
                if command==0x20:return encode_network(fixture()['network'])+b'\x01\x00\x00'
                if command==0x21:return b'\x01\x00'
                raise AssertionError(command)
        with self.assertRaises(ValueError):freeze_and_export(Peer(),fixture())

    def test_freeze_cli_offline_and_execution_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'template.json';p.write_text(json.dumps(fixture()))
            cmd=[os.sys.executable,str(ROOT/'scripts/ziggurat_freeze.py'),'--template',str(p)]
            self.assertEqual(subprocess.run(cmd,stdout=subprocess.DEVNULL).returncode,0)
            self.assertNotEqual(subprocess.run(cmd+['--execute'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode,0)

    def test_maintenance_cobs_hello_fragmentation_and_insecure_socket(self):
        binary=ROOT.parent/'ziggurat-compilation/build/gateway-maintenance-host'
        with tempfile.TemporaryDirectory() as tmp:
            path=str(Path(tmp)/'api.sock')
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path);server.listen();os.chmod(path,0o600)
                def send():
                    with server.accept()[0] as client:
                        for b in encode(b'\x00\x03\x00\x02\x00'):
                            client.sendall(bytes([b]));time.sleep(.005)
                worker=threading.Thread(target=send);worker.start()
                self.assertEqual(subprocess.run([str(binary),'cobs-hello',path]).returncode,0);worker.join()
                os.chmod(path,0o666)
                self.assertNotEqual(subprocess.run([str(binary),'cobs-hello',path]).returncode,0)


if __name__=='__main__':unittest.main()
