#!/usr/bin/env python3
"""Actual Ziggurat import against loopback RCP, using synthetic state only."""
import argparse
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from test_network_migration import fixture
from ziggurat_migration import prepared_state, verify_import, freeze_and_export, rollback_state


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment",type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument("--binary",type=Path)
    parser.add_argument("--state",type=Path,help="Private actual snapshot; loopback verification only, never starts the network")
    parser.add_argument("--freeze-running",action="store_true",help="Synthetic-only start/freeze/rollback scenario")
    args=parser.parse_args()
    if args.freeze_running and args.state:parser.error("running scenario only accepts synthetic state")
    spec=importlib.util.spec_from_file_location("sim",args.experiment/"ziggurat/scripts/simulate-rcp.py")
    sim=importlib.util.module_from_spec(spec);spec.loader.exec_module(sim)
    binary=args.binary or args.experiment/"build/host/debug/ziggurat"
    source=json.loads(args.state.read_text()) if args.state else fixture()
    rcp=sim.Rcp("127.0.0.1",iid=1)
    listener=socket.socket();listener.bind(("127.0.0.1",0));port=listener.getsockname()[1];listener.close()
    process=client=None
    report={"physical_radio_accessed":False,"firmware_flashed":False,"synthetic_state_only":args.state is None}
    try:
        with tempfile.TemporaryFile() as log:
            process=subprocess.Popen([str(binary),"--device",f"tcp://127.0.0.1:{rcp.port}","--spinel-iid","1","--listen",f"127.0.0.1:{port}","--log-level","error"],stdout=log,stderr=log)
            deadline=time.monotonic()+5
            while True:
                try:client=sim.Client(port,[]);break
                except OSError:
                    if process.poll() is not None or time.monotonic()>deadline:raise RuntimeError("host server startup failed")
                    time.sleep(0.05)
            state=prepared_state(source)
            report.update(verify_import(client,state))
            assert rcp.counts.get("tx_frames",0)==0
            report["radio_transmissions"]=rcp.counts.get("tx_frames",0)
            if args.freeze_running:
                client.request(0x16)
                deadline=time.monotonic()+2
                while rcp.counts.get("tx_frames",0)==0 and time.monotonic()<deadline:time.sleep(0.01)
                assert rcp.counts.get("tx_frames",0)>0
                final=freeze_and_export(client,state)
                # Frozen stacks cannot transmit, mutate keys, or restart without configure.
                client.request(0x16,status=0x11)
                client.request(0x11,b"\x00\x00",status=0x11)
                client.request(0x04) # idempotent
                second=freeze_and_export(client,state)
                assert final["network"]==second["network"] and final["keys"]==second["keys"]
                restored=rollback_state(source,final)
                assert restored["network"]["network_key_tx_counter"]>final["network"]["network_key_tx_counter"]
                assert restored["network"]["aps_frame_counter"]>final["network"]["aps_frame_counter"]
                transmitted=rcp.counts.get("tx_frames",0)
                time.sleep(0.05)
                assert rcp.counts.get("tx_frames",0)==transmitted
                report["network_started"]=True
                report["radio_transmissions"]=transmitted
                report["transmissions_stopped_after_freeze"]=True
                report["running_freeze_rollback_verified"]=True
            report["passed"]=True
    finally:
        if client:client.close()
        if process:
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        rcp.close()
    if rcp.error:raise RuntimeError("simulator failed")
    print(json.dumps(report,indent=2))


if __name__=="__main__":main()
