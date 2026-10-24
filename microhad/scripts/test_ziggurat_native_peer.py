#!/usr/bin/env python3
"""Scripted Unix COBS integration tests for the native service; synthetic only."""
from collections import deque
import copy
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

from test_network_migration import fixture
from ziggurat_migration import prepared_state, native_state
from ziggurat_cobs import encode, decode
from test_ziggurat_service import free_port, wait_for

ROOT=Path(__file__).resolve().parents[1]
NATIVE_BINARY=Path(os.environ.get("MICROHAD_TEST_BINARY", ROOT/"build/host-ziggurat/microhad"))


class Peer:
    def __init__(self,path,state):
        self.path=path;self.saved=copy.deepcopy(state);self.network=bytes.fromhex(state["network"])
        self.tables={0x21:bytes.fromhex(state["keys"]),0x22:bytes.fromhex(state["children"]),0x23:bytes.fromhex(state["addresses"])}
        self.configured=False;self.started=False;self.conn=None;self.stop=threading.Event();self.error=None
        self.lock=threading.Lock();self.commands=[];self.reject_send=False;self.reject_configure=False;self.fail_confirmation=False;self.drop_commands=set();self.omit_scan_rows=set()
        self.listener=socket.socket(socket.AF_UNIX);self.listener.bind(str(path));path.chmod(0o600);self.listener.listen();self.listener.settimeout(.1)
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()

    def send(self,frame,fragment=False):
        data=encode(frame)
        with self.lock:
            if not self.conn:return
            try:
                if fragment:
                    for i in range(0,len(data),2):self.conn.sendall(data[i:i+2])
                else:self.conn.sendall(data)
            except (BrokenPipeError,ConnectionResetError):pass

    def notify(self,command,payload):self.send(bytes([command,3,0])+payload)

    def aps(self,cluster,data,profile=0x0104):
        payload=struct.pack("<HHBHHHBBBbH",0x1234,0,0,0,profile,cluster,1,1,190,-42,len(data))+data
        self.notify(0x10,payload)

    def handle(self,raw):
        command=raw[0];bits=int.from_bytes(raw[1:3],"little");assert bits&3==0
        ident=bits>>2;body=raw[3:];self.commands.append((command,body))
        if command in self.drop_commands:return
        reply=bytes([command])+struct.pack("<H",ident<<2|1)
        status=0;response=b""
        if command==2:response=self.network[14:22]
        elif command==0x20:
            if not self.configured:status=0x10
            else:response=self.network+self.tables[0x21][:2]+bytes([self.started])
        elif command==0x10:
            assert len(body)==84
            committed=json.loads((self.path.parent/"ziggurat-state.json").read_text())
            assert committed["network"]==body[2:].hex() # Commit precedes Configure.
            if self.reject_configure:status=0x20
            else:self.network=body[2:];self.configured=True;self.started=False
        elif command in (0x11,0x12,0x13):self.tables[command+0x10]=body
        elif command==0x16:self.started=True
        elif command==4:self.started=False
        elif command in (0x21,0x22,0x23):
            table=self.tables[command];width={0x21:33,0x22:11,0x23:10}[command]
            for at in ([] if command in self.omit_scan_rows else range(2,len(table),width)):self.send(bytes([command])+struct.pack("<H",ident<<2|2)+table[at:at+width])
            response=table[:2]
        elif command in (0x30,0x31,0x32):
            if self.reject_send:status=0x20
        elif command!=0x34:status=2
        self.send(reply+bytes([status])+response,fragment=True)
        if command in (0x30,0x31,0x32) and not status:
            # Group sends have one completion; APS-acked unicast has two stages.
            if command==0x30:self.send(b"\x11"+struct.pack("<H",ident<<2|3)+b"\x00")
            final=0x12 if command==0x30 else 0x13
            self.send(bytes([final])+struct.pack("<H",ident<<2|3)+bytes([7 if self.fail_confirmation else 0]))
            if command==0x30 and not self.fail_confirmation:
                profile=int.from_bytes(body[11:13],"little");cluster=int.from_bytes(body[13:15],"little");asdu=body[23:]
                if profile==0 and asdu:
                    if cluster==5:self.aps(0x8005,asdu[:1]+b"\x00\x34\x12\x01\x01",profile=0)
                    elif cluster==4:
                        descriptor=b"\x01\x04\x01\x00\x01\x00\x02\x06\x00\x02\x04\x00"
                        self.aps(0x8004,asdu[:1]+b"\x00\x34\x12"+bytes([len(descriptor)])+descriptor,profile=0)
                    elif cluster==0x21:self.aps(0x8021,asdu[:1]+b"\x00",profile=0)

    def run(self):
        try:
            while not self.stop.is_set():
                try:connection,_=self.listener.accept()
                except socket.timeout:continue
                self.conn=connection;connection.settimeout(.1);buffer=bytearray()
                self.send(bytes([0,3,0,2,int(self.configured)]),fragment=True)
                while not self.stop.is_set():
                    try:chunk=connection.recv(4096)
                    except socket.timeout:continue
                    except ConnectionResetError:break
                    if not chunk:break
                    buffer.extend(chunk)
                    while 0 in buffer:
                        at=buffer.index(0);frame=bytes(buffer[:at]);del buffer[:at+1]
                        if frame:self.handle(decode(frame))
                connection.close();self.conn=None
        except (OSError,AssertionError,ValueError) as error:
            if not self.stop.is_set():self.error=repr(error)

    def close(self):
        self.stop.set()
        if self.conn:
            try:self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:pass
        self.thread.join(timeout=2);self.listener.close()


class NativeTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix="native-cobs-peer-");self.directory=Path(self.tmp.name)
        self.state=native_state(prepared_state(fixture()));self.path=self.directory/"api.sock";self.state_path=self.directory/"ziggurat-state.json"
        self.state_path.write_text(json.dumps(self.state));self.state_path.chmod(0o600)
        self.port=free_port();self.config=self.directory/"config.json"
        self.config.write_text(json.dumps({"serial_device":"/dev/forbidden-radio", "runtime_dir":str(self.directory),"http_port":self.port,
            "https_port":free_port(),"ziggurat_socket":str(self.path)}));self.config.chmod(0o600)
        self.log=tempfile.TemporaryFile();self.process=None;self.peer=Peer(self.path,self.state)
        self.launch()

    def launch(self):
        self.process=subprocess.Popen([str(NATIVE_BINARY),"--config",str(self.config)],stdout=self.log,stderr=self.log)
        wait_for(lambda:self.status()["online"])

    def request(self,path,data=None):
        request=urllib.request.Request(f"http://127.0.0.1:{self.port}"+path,data=data)
        with urllib.request.urlopen(request,timeout=2) as response:return json.load(response)

    def status(self):return self.request("/api/status")
    def saved(self):return json.loads(self.state_path.read_text())

    def tearDown(self):
        if self.process:
            self.process.terminate()
            try:self.process.wait(timeout=7)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
        self.peer.close();self.log.close();self.tmp.cleanup()
        self.assertIsNone(self.peer.error)

    def test_fragmented_startup_and_join(self):
        self.assertEqual(self.status()["coordinator_backend"],"ziggurat")
        self.assertTrue(self.request("/api/join",b"")["ok"])
        wait_for(lambda:any(command==0x34 for command,_ in self.peer.commands))
        payload=next(body for command,body in self.peer.commands if command==0x34)
        self.assertEqual(payload,b"\x2c\x01\x01")

    def submit_network(self):
        # The broadcast threshold request may still be finishing after Ready.
        def submit():
            try:return self.request('/api/network/create', b'confirmation=DELETE')['ok']
            except urllib.error.HTTPError as error:
                self.assertEqual(error.code,400)
                self.assertIn('busy',json.load(error)['error'])
                return False
        wait_for(submit)

    def create_network(self):
        previous=self.saved()['network'] if self.state_path.exists() else None
        self.submit_network()
        wait_for(lambda:self.state_path.exists() and self.saved()['network']!=previous)
        wait_for(lambda:self.status()['network_formed'] and not self.status()['network_forming'])

    def test_replace_network_preserves_old_snapshot_and_channel(self):
        previous=self.peer.network
        self.create_network()
        current=bytes.fromhex(self.saved()['network'])
        self.assertEqual(current[0],previous[0])
        self.assertEqual(current[14:22],previous[14:22])
        self.assertNotEqual(current[4:12],previous[4:12])
        self.assertNotEqual(current[22:38],previous[22:38])
        self.assertEqual(self.saved()['keys'],'0000')
        self.assertEqual(self.saved()['children'],'0000')
        self.assertEqual(self.saved()['addresses'],'0000')
        backups=list(self.directory.glob('ziggurat-state.json.before-network-create-*'))
        self.assertEqual(len(backups),1)
        backup=json.loads(backups[0].read_text())
        self.assertEqual(bytes.fromhex(backup['network'])[4:38],previous[4:38])
        self.assertEqual(backups[0].stat().st_mode & 0o777,0o600)
        self.assertTrue(any(command==4 for command,_ in self.peer.commands))
        configured=sum(command==0x10 for command,_ in self.peer.commands)
        self.process.kill();self.process.wait();self.process=None
        self.launch()
        self.assertEqual(bytes.fromhex(self.saved()['network'])[4:38],current[4:38])
        self.assertEqual(sum(command==0x10 for command,_ in self.peer.commands),configured)

    def test_create_network_without_saved_state_requires_confirmation(self):
        self.process.terminate();self.process.wait(timeout=7);self.process=None
        self.state_path.unlink()
        self.peer.configured=False;self.peer.started=False
        self.launch()
        self.assertFalse(self.status()['network_formed'])
        self.assertFalse(self.state_path.exists())
        with self.assertRaises(urllib.error.HTTPError) as rejected:
            self.request('/api/network/create',b'confirmation=no')
        self.assertEqual(rejected.exception.code,400)
        self.assertFalse(self.state_path.exists())
        self.create_network()
        self.assertTrue(self.state_path.exists())
        self.assertFalse(list(self.directory.glob('ziggurat-state.json.before-network-create-*')))

    def test_replacement_checkpoint_failure_preserves_identity(self):
        before=self.saved()
        configured=sum(command==0x10 for command,_ in self.peer.commands)
        self.peer.omit_scan_rows.add(0x21)
        self.submit_network()
        wait_for(lambda:not self.status()['online'])
        self.assertEqual(self.saved(),before)
        self.assertEqual(sum(command==0x10 for command,_ in self.peer.commands),configured)

    def test_missing_state_does_not_replace_unknown_live_network(self):
        self.process.kill();self.process.wait();self.process=None
        self.state_path.unlink()
        configured=sum(command==0x10 for command,_ in self.peer.commands)
        self.process=subprocess.Popen([str(NATIVE_BINARY),'--config',str(self.config)],stdout=self.log,stderr=self.log)
        wait_for(lambda:any('network identity mismatch' in line for line in self.request('/api/logs')['lines']))
        self.assertFalse(self.status()['online'])
        self.assertFalse(self.state_path.exists())
        self.assertEqual(sum(command==0x10 for command,_ in self.peer.commands),configured)

    def test_repeated_replacement_keeps_existing_backups(self):
        for index in range(2):
            self.create_network()
        backups=list(self.directory.glob('ziggurat-state.json.before-network-create-*'))
        self.assertEqual(len(backups),2)
        self.assertNotEqual(backups[0].read_bytes(),backups[1].read_bytes())

    def test_replacement_storage_failure_never_sends_configure(self):
        before=self.saved()
        configured=sum(command==0x10 for command,_ in self.peer.commands)
        self.state_path.chmod(0o644)
        self.submit_network()
        wait_for(lambda:not self.status()['online'])
        self.assertEqual(self.saved(),before)
        self.assertEqual(sum(command==0x10 for command,_ in self.peer.commands),configured)
        self.assertTrue(list(self.directory.glob('ziggurat-state.json.before-network-create-*')))

    def test_replacement_configure_rejected_keeps_backup_and_stays_offline(self):
        previous=self.peer.network
        self.peer.reject_configure=True
        self.submit_network()
        wait_for(lambda:not self.status()['online'])
        backup=next(self.directory.glob('ziggurat-state.json.before-network-create-*'))
        self.assertEqual(bytes.fromhex(json.loads(backup.read_text())['network'])[4:38],previous[4:38])
        self.assertNotEqual(bytes.fromhex(self.saved()['network'])[22:38],previous[22:38])
        self.assertFalse(self.status()['network_formed'])
        self.assertFalse(self.peer.started)

    def test_counter_and_link_key_notifications_are_durable(self):
        nwk=struct.pack("<I",50000);aps=struct.pack("<I",60000)
        self.peer.network=self.peer.network[:39]+nwk+self.peer.network[43:78]+aps
        self.peer.notify(0x30,nwk);self.peer.notify(0x31,aps)
        ieee=bytes.fromhex(fixture()["keys"][0]["ieee"]);key=bytes(range(64,80))
        row=key+aps+struct.pack("<I",43)+b"\x00"+ieee;self.peer.tables[0x21]=b"\x01\x00"+row
        self.peer.notify(0x32,ieee+key)
        wait_for(lambda:bytes.fromhex(self.saved()["keys"])[2:18]==key)
        saved=self.saved();network=bytes.fromhex(saved["network"])
        self.assertEqual(network[39:43],nwk);self.assertEqual(network[78:82],aps)
        self.process.kill();self.process.wait();self.process=None
        self.peer.configured=False;self.peer.started=False
        self.launch();network=bytes.fromhex(self.saved()["network"])
        self.assertGreaterEqual(int.from_bytes(network[39:43],"little"),51000)
        self.assertGreaterEqual(int.from_bytes(network[78:82],"little"),61000)
        self.assertTrue(bytes.fromhex(self.saved()["keys"])[2:18]==key)

    def test_received_aps_interview_reports_and_light_send(self):
        ieee=bytes.fromhex(fixture()["keys"][0]["ieee"])
        self.peer.notify(0x20,b"\x34\x12"+ieee+b"\x00\x00\x04")
        wait_for(lambda:self.status()["devices"])
        wait_for(lambda:any(command==0x30 and body[13:15]==b"\x04\x00" for command,body in self.peer.commands))
        self.peer.aps(0x0402,b"\x18\x42\x0a\x00\x00\x29"+struct.pack("<h",2150))
        wait_for(lambda:any(d["temperature_celsius"]==21.5 for d in self.status()["devices"]))
        self.peer.aps(0x0006,b"\x18\x43\x0a\x00\x00\x10\x01")
        device=wait_for(lambda:next((d for d in self.status()["devices"] if d["has_light"]),None))
        response=self.request("/api/devices/"+device["ieee_address"]+"/light",b"state=off")
        self.assertTrue(response["ok"])
        wait_for(lambda:any(command==0x30 and body[13:15]==b"\x06\x00" for command,body in self.peer.commands))
        self.peer.aps(0x0002,b"\x55\x00\x00",profile=0)
        wait_for(lambda:any(command==0x30 and body[13:15]==b"\x02\x80" for command,body in self.peer.commands))
        self.peer.notify(0x21,b"\x34\x12\x01"+ieee+b"\x00\xff\xff"+bytes(8))
        wait_for(lambda: any(d["network_address"]==65535 for d in json.loads((self.directory/"devices.json").read_text())["devices"]))

    def test_malformed_and_oversized_frames_resynchronize(self):
        with self.peer.lock:self.peer.conn.sendall(b"\x02\x00"+b"\x01"*16385+b"\x00")
        self.peer.notify(0x30,struct.pack("<I",50000))
        wait_for(lambda:int.from_bytes(bytes.fromhex(self.saved()["network"])[39:43],"little")==50000)
        self.assertTrue(self.status()["online"])

    def test_storage_failure_requests_freeze_and_stays_offline(self):
        self.state_path.chmod(0o644)
        self.peer.notify(0x30,struct.pack("<I",50000))
        wait_for(lambda:not self.status()["online"])
        wait_for(lambda:any(command==4 for command,_ in self.peer.commands))
        time.sleep(.05);self.assertFalse(self.status()["online"])

    def test_reconnect_preserves_running_stack(self):
        configured=sum(command==0x10 for command,_ in self.peer.commands)
        with self.peer.lock:self.peer.conn.shutdown(socket.SHUT_RDWR)
        wait_for(lambda:not self.status()["online"])
        wait_for(lambda:self.status()["online"],timeout=5)
        self.assertEqual(sum(command==0x10 for command,_ in self.peer.commands),configured)

    def test_admission_and_confirmation_failures_are_reported(self):
        ieee=bytes.fromhex(fixture()["keys"][0]["ieee"])
        self.peer.reject_send=True
        self.peer.notify(0x20,b"\x34\x12"+ieee+b"\x00\x00\x04")
        wait_for(lambda:any(command==0x30 for command,_ in self.peer.commands))
        wait_for(lambda:any("Ziggurat send failed" in line for line in self.request("/api/logs")["lines"]))
        self.assertTrue(self.status()["online"])

    def test_control_timeout_closes_join_window(self):
        self.peer.drop_commands.add(0x34)
        self.assertTrue(self.request("/api/join",b"")["ok"])
        wait_for(lambda:self.status()["joining_until"]>0)
        wait_for(lambda:self.status()["joining_until"]==0,timeout=12)

    def test_live_identity_mismatch_is_not_overwritten(self):
        saved=self.saved()
        n=bytearray(self.peer.network);n[14]^=1;self.peer.network=bytes(n)
        with self.peer.lock:self.peer.conn.shutdown(socket.SHUT_RDWR)
        wait_for(lambda:not self.status()["online"])
        time.sleep(1.3)
        self.assertFalse(self.status()["online"])
        self.assertTrue(self.saved()==saved)

    def test_malformed_counter_notification_fails_closed(self):
        self.peer.notify(0x30,b"\x01\x02")
        wait_for(lambda:not self.status()["online"])
        self.assertFalse(self.status()["network_formed"])

    def test_partial_scan_never_overwrites_saved_keys(self):
        saved=self.saved();self.peer.omit_scan_rows.add(0x21)
        ieee=bytes.fromhex(fixture()["keys"][0]["ieee"])
        self.peer.notify(0x20,b"\x34\x12"+ieee+b"\x00\x00\x04")
        wait_for(lambda:not self.status()["online"])
        self.assertTrue(self.saved()["keys"]==saved["keys"])

    def test_crash_after_counter_commit_before_configure(self):
        self.process.kill();self.process.wait();self.process=None
        self.peer.configured=False;self.peer.started=False;self.peer.drop_commands.add(0x10)
        first=int.from_bytes(bytes.fromhex(self.saved()["network"])[39:43],"little")
        self.process=subprocess.Popen([str(NATIVE_BINARY),"--config",str(self.config)],stdout=self.log,stderr=self.log)
        wait_for(lambda:int.from_bytes(bytes.fromhex(self.saved()["network"])[39:43],"little")>first)
        committed=self.saved()
        self.process.kill();self.process.wait();self.process=None;self.peer.drop_commands.clear()
        self.launch()
        restored=bytes.fromhex(self.saved()["network"]);before=bytes.fromhex(committed["network"])
        self.assertGreater(int.from_bytes(restored[39:43],"little"),int.from_bytes(before[39:43],"little"))
        self.assertGreater(int.from_bytes(restored[78:82],"little"),int.from_bytes(before[78:82],"little"))

    def test_corrupt_state_blocks_startup(self):
        self.process.kill();self.process.wait();self.process=None
        self.state_path.write_text("{}");self.state_path.chmod(0o600)
        process=subprocess.Popen([str(NATIVE_BINARY),"--config",str(self.config)],stdout=self.log,stderr=self.log)
        self.assertEqual(process.wait(timeout=5),1)


if __name__=="__main__":unittest.main()
