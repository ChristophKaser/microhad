#!/usr/bin/env python3
"""Actual Ziggurat Unix COBS tests, host or gateway, exclusively with a host RCP simulator.

Only Ziggurat and a byte-stream probe are uploaded. No service/config/UART/firmware
changes. Remote uploads are removed on exit; state and traffic are synthetic.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request

from ziggurat_cobs import Client, encode

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("sim",ROOT/"scripts/simulate-rcp.py")
sim=importlib.util.module_from_spec(spec);spec.loader.exec_module(sim)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host")
    parser.add_argument("--password-file",type=Path,default=ROOT.parent/"ssh_password.txt")
    parser.add_argument("--binary",type=Path)
    parser.add_argument("--upload-storage",choices=["ram","flash"],default="ram",
        help="Temporary executable staging; flash avoids ramfs allocation and is removed after testing")
    args=parser.parse_args();token=str(time.time_ns())
    radio=sim.Rcp(args.host or "127.0.0.1",tcp_nodelay=True,iid=1)
    process=bridge=client=None;output=[];remote_dir=None
    report={"target":args.host or "host","synthetic_state_only":True,"physical_radio_accessed":False,
        "service_deployed":False,"firmware_flashed":False,"results":[]}
    ssh=sim.ssh_args(args) if args.host else []
    def remote(command,**kw):return subprocess.check_output(ssh+[f"root@{args.host}",command],text=True,timeout=15,**kw)
    def passed(text):report["results"].append(text);print("PASS",text,flush=True)
    def status():
        with urllib.request.urlopen(f"http://{args.host}/api/status",timeout=8) as response:
            value=json.load(response);return {"online":value["online"],"network_formed":value["network_formed"],"device_count":len(value["devices"])}
    def upload(path,destination):
        with path.open("rb") as source:
            subprocess.run(ssh+[f"root@{args.host}",f"umask 077; test ! -e {shlex.quote(destination)} && cat > {shlex.quote(destination)} && chmod 700 {shlex.quote(destination)}"],stdin=source,check=True,timeout=45)
    try:
        with tempfile.TemporaryDirectory(prefix="cobs-unix-host-") as tmp, tempfile.TemporaryFile() as log:
            if args.host:
                report["production_before"]=status();assert report["production_before"]["online"] and report["production_before"]["network_formed"]
                report["production_binary_before"]=remote("md5sum /tuya/native/tuya-zigbee-native").split()[0]
                binary=args.binary or ROOT/"build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped"
                probe=ROOT/"build/hdlc-unix-probe"
                memory=remote("cat /proc/meminfo")
                available=next(int(line.split()[1]) for line in memory.splitlines() if line.startswith("MemFree:"))
                upload_kib=(binary.stat().st_size+probe.stat().st_size+1023)//1024
                required=8192+(upload_kib if args.upload_storage=="ram" else 0)
                report["memory_preflight_kib"]={"available":available,"required":required}
                if available<required:raise RuntimeError("Insufficient free RAM for safe simulation")
                root="/tmp" if args.upload_storage=="ram" else "/tuya"
                remote_dir=f"{root}/ziggurat-simulation-{token}"
                if root=="/tuya":
                    space=remote("df -k /tuya").splitlines()[-1].split();free=int(space[-3])
                    report["flash_preflight_kib"]={"available":free,"required":upload_kib+1024}
                    if free<upload_kib+1024:raise RuntimeError("Insufficient staging flash space")
                remote(f"umask 077; mkdir {remote_dir}")
                upload(binary,remote_dir+"/ziggurat");upload(probe,remote_dir+"/probe")
                # Probe permits this unique experiment prefix; private directory protects socket.
                socket_dir=f"/tmp/ziggurat-hdlc-cobs-{token}"
                socket_path=socket_dir+"/api.sock";top_path=socket_dir+"/top.log"
                command=f'''set -u
umask 077
mkdir {socket_dir} || exit 2
ulimit -c 0
{remote_dir}/ziggurat --device tcp://{radio.address}:{radio.port} --spinel-iid 1 --api cobs-unix --listen unix:{socket_path} --log-level error > {socket_dir}/server.log 2>&1 &
child=$!
echo TOP_PID "$child"
top -b -d 1 -n 35 > {top_path} 2>&1 &
top_pid=$!
(sleep 40; kill -9 "$child" 2>/dev/null) &
watchdog=$!
cleanup() {{
 kill "$child" "$top_pid" "$watchdog" 2>/dev/null
 wait "$child" 2>/dev/null
 wait "$top_pid" 2>/dev/null
 wait "$watchdog" 2>/dev/null
 cat {socket_dir}/server.log
 awk '/^CPU:/ {{frame++; print "TOP_CPU",frame,$0}} $1 ~ /^[0-9]+$/ && NF >= 8 {{print "TOP_ROW",frame,$1,$2,$7,$5}}' {top_path}
 rm -f {socket_path} {top_path} {socket_dir}/server.log {remote_dir}/ziggurat {remote_dir}/probe
 rm -rf {socket_dir} {remote_dir}
}}
trap cleanup EXIT HUP INT TERM
sleep 1
kill -0 "$child" || exit 1
echo READY
while read action; do
 case "$action" in
 sample) awk '/^(VmSize|VmRSS|Threads):/' /proc/$child/status; awk '/^MemFree:/ {{print}}' /proc/meminfo ;;
 stop) exit 0 ;;
 esac
done
'''
                process=subprocess.Popen(ssh+[f"root@{args.host}","sh -c "+shlex.quote(command)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
                ready=threading.Event()
                def reader():
                    for line in process.stdout:
                        output.append(line.rstrip())
                        if line.strip()=="READY":ready.set()
                thread=threading.Thread(target=reader,daemon=True);thread.start()
                if not ready.wait(12):raise RuntimeError("Remote simulation failed to start")
                bridge=subprocess.Popen(ssh+[f"root@{args.host}",remote_dir+"/probe",socket_path],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
                client=Client(bridge.stdout,bridge.stdin)
            else:
                socket_path=Path(tmp)/"api.sock";binary=args.binary or ROOT/"build/host/debug/ziggurat"
                process=subprocess.Popen([str(binary),"--device",f"tcp://127.0.0.1:{radio.port}","--spinel-iid","1","--api","cobs-unix","--listen",f"unix:{socket_path}","--log-level","error"],stdout=log,stderr=log)
                end=time.monotonic()+5
                while not socket_path.exists():
                    if process.poll() is not None or time.monotonic()>end:raise RuntimeError("Host simulation failed to start")
                    time.sleep(.02)
                client=Client.connect(socket_path)
            assert client.request(1)[0]==2
            assert client.request(2)==sim.EUI
            passed("Protocol-v2 Hello, firmware info and hardware address over Unix COBS")
            client.request(0x20,status=0x10)
            original=client.write
            def fragmented(data):
                for at in range(0,len(data),2):original(data[at:at+2])
            client.write=fragmented
            assert client.request(1)[0]==2;client.write=original
            passed("Fragmented COBS requests preserve message boundaries")
            client.write(b"\x02\x00"+b"\x01"*16385+b"\x00")
            assert client.request(1)[0]==2
            client.request(0xfe,status=2)
            passed("Malformed/oversized frames resynchronize and unknown commands fail explicitly")
            client.request(0x42,b"\x0f");radio.inject(bytes.fromhex("01087e7d1113f8"))
            captured=client.receive();assert captured[0]==0x42 and captured[1]&3==2
            passed("Simulated RCP receive reaches COBS API with escaped bytes intact")
            client.request(0x10,sim.config());client.request(0x16)
            assert client.request(0x20)[84]==1
            passed("Synthetic network config/start traverses IID 1 to the host RCP")
            broadcast=struct.pack("<BHHHBBBBbH",0,0xfffc,0x104,6,1,1,0,3,0,4)+b"test"
            for _ in range(3):client.request(0x31,broadcast)
            passed("APS broadcasts are admitted on the simulated network")
            start=len(client.timings)
            for _ in range(200):client.request(0x20);time.sleep(.02)
            report["query_timing_ms"]=sim.timing_summary(client.timings[start:])
            report["simulator_response_ms"]=sim.timing_summary(radio.response_times_ms)
            if args.host:process.stdin.write("sample\n");process.stdin.flush();time.sleep(.1)
            client.request(4);assert client.request(0x20)[84]==0
            client.request(0x16,status=0x11)
            passed("Freeze stops the stack and refuses an implicit restart")
            if args.host:
                bridge.terminate();bridge.wait(timeout=5);bridge=None;client=None
                process.stdin.write("stop\n");process.stdin.flush();process.wait(timeout=8);thread.join(timeout=2);process=None
                report["top_samples"]=sim.top_samples(output)
                report["memory_samples"]=[line for line in output if line.startswith(("VmRSS:","VmSize:","MemFree:"))]
                report["production_after"]=status()
                report["production_binary_after"]=remote("md5sum /tuya/native/tuya-zigbee-native").split()[0]
                assert report["production_binary_before"]==report["production_binary_after"]
                assert report["production_after"]["online"] and report["production_after"]["network_formed"]
                assert remote(f"test ! -e {remote_dir} && test ! -e {socket_dir} && echo CLEAN").strip()=="CLEAN"
                report["temporary_uploads_removed"]=True
                passed("Production network/service unchanged and temporary uploads removed")
            report["simulated_radio_transmissions"]=radio.counts["tx_frames"]
    finally:
        if bridge:
            bridge.terminate()
            try:bridge.wait(timeout=5)
            except subprocess.TimeoutExpired:bridge.kill();bridge.wait()
        if client:client.close()
        if process:
            if args.host:
                try:process.stdin.write("stop\n");process.stdin.flush();process.wait(timeout=8)
                except (BrokenPipeError,subprocess.TimeoutExpired):process.terminate();process.wait(timeout=5)
            else:process.terminate();process.wait(timeout=5)
        if args.host and remote_dir:
            # Exact files owned by this invocation; also covers partial upload failures.
            remote(f"rm -f {remote_dir}/ziggurat {remote_dir}/probe; rm -rf {remote_dir}")
        radio.close()
    if radio.error:raise RuntimeError("RCP simulator failed")
    (ROOT/"logs").mkdir(exist_ok=True)
    (ROOT/"logs"/f"cobs-unix-{token}.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report,indent=2))

if __name__=="__main__":main()
