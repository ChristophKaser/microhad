#!/usr/bin/env python3
"""Native service + actual Ziggurat + loopback RCP; synthetic state only."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request

from test_network_migration import fixture
from ziggurat_migration import prepared_state, native_state
from ziggurat_cobs import Client, encode

ROOT = Path(__file__).resolve().parents[1]


def wait_for(check, timeout=12):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            value=check()
            if value: return value
        except (OSError, ValueError): pass
        time.sleep(0.025)
    raise TimeoutError("host integration condition")


def free_port():
    with socket.socket() as stream:
        stream.bind(("127.0.0.1",0)); return stream.getsockname()[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment",type=Path,default=ROOT.parent)
    args=parser.parse_args()
    spec=importlib.util.spec_from_file_location("sim",args.experiment/"ziggurat/scripts/simulate-rcp.py")
    sim=importlib.util.module_from_spec(spec);spec.loader.exec_module(sim)
    radio=sim.Rcp("127.0.0.1",iid=1)
    native=ROOT/"build/host-ziggurat/microhad"
    server=args.experiment/"build/host/debug/ziggurat"
    process=service=observer=None
    report={"synthetic_state_only":True,"physical_radio_accessed":False,"service_deployed":False,"results":[]}
    def passed(text):report["results"].append(text);print("PASS",text,flush=True)
    try:
        with tempfile.TemporaryDirectory(prefix="native-ziggurat-") as tmp, tempfile.TemporaryFile() as log:
            directory=Path(tmp);path=directory/"api.sock";state_path=directory/"ziggurat-state.json"
            state=native_state(prepared_state(fixture()))
            state_path.write_text(json.dumps(state));state_path.chmod(0o600)
            config_path=directory/"config.json";port=free_port()
            config_path.write_text(json.dumps({"serial_device":"/dev/forbidden-physical-radio", "runtime_dir":tmp,
                "ziggurat_socket":str(path),"http_port":port,"https_port":free_port()}));config_path.chmod(0o600)
            process=subprocess.Popen([str(server),"--device",f"tcp://127.0.0.1:{radio.port}","--spinel-iid","1",
                "--api","cobs-unix","--listen",f"unix:{path}","--log-level","error"],stdout=log,stderr=log)
            wait_for(lambda:path.exists());assert path.stat().st_mode&0o777==0o600
            def launch():return subprocess.Popen([str(native),"--config",str(config_path)],stdout=log,stderr=log)
            def status():
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/status",timeout=1) as response:return json.load(response)
            service=launch()
            try:
                current=wait_for(lambda: (s if (s:=status())["online"] else None))
            except Exception:
                log.seek(0);print(log.read().decode(errors="replace")[-5000:]);raise
            assert current["network_formed"] and current["coordinator_backend"]=="ziggurat"
            saved=json.loads(state_path.read_text());network=bytes.fromhex(saved["network"])
            assert int.from_bytes(network[39:43],"little")>=int.from_bytes(bytes.fromhex(state["network"])[39:43],"little")+1000
            assert int.from_bytes(network[78:82],"little")>=int.from_bytes(bytes.fromhex(state["network"])[78:82],"little")+1000
            expected_key=bytes.fromhex(state["keys"])[2:]
            restored_key=bytes.fromhex(saved["keys"])[2:]
            assert restored_key[:16]==expected_key[:16] and restored_key[20:]==expected_key[20:]
            assert saved["children"]==state["children"] and saved["addresses"]==state["addresses"]
            passed("Native COBS startup restores synthetic identity, keys and both counters")
            # Routers relay our encrypted parent broadcast with NWK source 0 and
            # their own MAC source. It must not evict our restored sleepy child.
            from cryptography.hazmat.primitives.ciphers.aead import AESCCM
            observer=Client.connect(path)
            def children():
                before=len(observer.events); reply=observer.request(0x22)
                rows=[f[3:] for f in observer.events[before:] if f[0]==0x22 and f[1]&3==2]
                assert len(rows)==int.from_bytes(reply,"little")
                return rows
            original_children=children(); assert len(original_children)==1
            child=bytes.fromhex(fixture()["children"][0]["ieee"])
            def parent_frame(cluster, nwk_source, sequence):
                nwk=struct.pack("<HHHBB",0x0208,0xfffc,nwk_source,30,sequence)
                sender=bytes.fromhex(fixture()["network"]["ieee_address"])
                aux=bytes([0x28])+struct.pack("<I",70000+sequence)+sender+bytes([7])
                authenticated_aux=bytes([0x2d])+aux[1:]
                asdu=bytes([sequence])+(bytes([0]) if cluster==0x801f else b"")+b"\x01"+child
                aps=bytes([8,0])+struct.pack("<HH",cluster,0)+bytes([0,sequence])+asdu
                nonce=sender+aux[1:5]+bytes([0x2d])
                ciphertext=AESCCM(bytes(range(16)),tag_length=4).encrypt(nonce,aps,nwk+authenticated_aux)
                mac=struct.pack("<HBHHH",0x8841,sequence,0x1a62,0xffff,0x2222)
                return mac+nwk+aux+ciphertext
            def observe_frame(cluster, source, sequence):
                radio.inject(parent_frame(cluster,source,sequence))
                while True:
                    frame=observer.receive()
                    if frame[0]==0x10 and frame[1]&3==3:
                        payload=frame[3:]
                        if int.from_bytes(payload[9:11],"little")==cluster:break
                return children()
            assert observe_frame(0x001f,0,47)==original_children
            assert observe_frame(0x801f,0,48)==original_children
            passed("Relayed own parent announcement and response preserve restored sleepy child")
            # Another router's legitimate claim must still remove an unconfirmed child.
            assert observe_frame(0x001f,0x2222,49)==[]
            observer.request(4); observer.close(); observer=None
            # Reconfigure from the synthetic snapshot for the remaining lifecycle cases.
            service.terminate();service.wait(timeout=10);service=None
            state_path.write_text(json.dumps(state));state_path.chmod(0o600)
            service=launch();wait_for(lambda:status()["online"])
            duplicate=subprocess.run([str(server),"--device",f"tcp://127.0.0.1:{radio.port}","--spinel-iid","1",
                "--api","cobs-unix","--listen",f"unix:{path}","--log-level","error"],stdout=log,stderr=log,timeout=5)
            assert duplicate.returncode!=0 and status()["online"]
            passed("Actual server refuses a live socket without replacing the running listener")
            with urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}/api/join",data=b"",method="POST"),timeout=2) as response:
                assert json.load(response)["ok"]
            passed("Native HTTP permit-join maps to Ziggurat")
            observer=Client.connect(path);info=observer.request(0x20);assert info[84]==1
            before=json.loads(state_path.read_text())
            service.terminate();service.wait(timeout=10);service=None
            info=observer.request(0x20);assert info[84]==0
            saved=json.loads(state_path.read_text());assert bytes.fromhex(saved["network"])==info[:82]
            passed("Orderly native stop freezes and persists actual Ziggurat state")
            template=directory/"template.json";template.write_text(json.dumps(fixture()));template.chmod(0o600)
            final=directory/"final-frozen.json"
            subprocess.run([os.sys.executable,str(ROOT/"scripts/ziggurat_freeze.py"),"--template",str(template),
                "--socket",str(path),"--quiesced","--execute","--output",str(final)],check=True,stdout=subprocess.DEVNULL)
            frozen=json.loads(final.read_text())
            assert frozen["handover"]["counters_observed_after_stop"] and final.stat().st_mode&0o777==0o600
            assert frozen["network"]["network_key_tx_counter"]==int.from_bytes(info[39:43],"little")
            assert len(frozen["keys"])==int.from_bytes(info[82:84],"little")
            passed("Recovery CLI exports complete private state after native shutdown/freeze")
            old=bytes.fromhex(saved["network"])
            service=launch();wait_for(lambda:status()["online"])
            info=observer.request(0x20);assert info[84]==1
            assert int.from_bytes(info[39:43],"little")>=int.from_bytes(old[39:43],"little")+1000
            assert int.from_bytes(info[78:82],"little")>=int.from_bytes(old[78:82],"little")+1000
            passed("Service restart reloads frozen stack with advanced NWK/APS counters")
            # A crash leaves the stack running: reconnect must not configure/start it again.
            service.kill();service.wait(timeout=3);service=None
            info=observer.request(0x20);old=info[:82];resets=radio.counts["resets"]
            service=launch();wait_for(lambda:status()["online"])
            info=observer.request(0x20);assert info[:82]==old and radio.counts["resets"]==resets
            passed("Native crash/reconnect attaches to the running stack without reset")
            observer.close();observer=None
            service.kill();service.wait(timeout=3);service=None
            process.terminate();process.wait(timeout=5);process=None
            radio.close();radio=sim.Rcp("127.0.0.1",iid=1)
            process=subprocess.Popen([str(server),"--device",f"tcp://127.0.0.1:{radio.port}","--spinel-iid","1",
                "--api","cobs-unix","--listen",f"unix:{path}","--log-level","error"],stdout=log,stderr=log)
            wait_for(lambda:path.exists());service=launch();wait_for(lambda:status()["online"])
            passed("Ziggurat restart reclaims stale socket and recovers native durable state")
            report["simulated_radio_transmissions"]=radio.counts["tx_frames"]
    finally:
        if observer:observer.close()
        if service:
            service.terminate()
            try:service.wait(timeout=10)
            except subprocess.TimeoutExpired:service.kill();service.wait()
        if process:
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        radio.close()
    if radio.error:raise RuntimeError("host radio simulator failed")
    print(json.dumps(report,indent=2))

if __name__=="__main__":main()
