#!/usr/bin/env python3
"""Exclusive EZSP v13 final read-only capture, optionally enter bootloader immediately.

Default validates the comparison export offline. Live mode requires the native
supervisor/daemon stopped and exactly one inspected serial bridge owning UART.
No flash, network formation, joining, key or counter write commands are issued.
"""
import argparse
import asyncio
from collections import deque
from datetime import datetime, timezone
import json
import logging
from pathlib import Path

from network_backup import normalize, private_write, ssh_args


def compare(original, final):
    for field in ('ieee_address', 'pan_id', 'extended_pan_id', 'channel', 'network_key',
                  'network_key_seq', 'tc_link_key', 'tclk_seed', 'tclk_flavor'):
        if original['network'][field] != final['network'][field]:
            raise ValueError('identity or security changed before exclusive handover')
    for field in ('network_key_tx_counter', 'aps_frame_counter'):
        if final['network'][field] < original['network'][field]:
            raise ValueError('final counter regressed')
    if not final['counter_evidence']['aps_live_upper_bound_verified']:
        raise ValueError('live APS counter evidence missing')


def raw_read_protocol(ezsp):
    # Error replies from this NCP can contain only status, while Bellows' typed
    # schemas expect a full row. Preserve bounded raw replies for our existing
    # strict exporter decoder instead of timing out on empty slots.
    class RawResponse:
        @classmethod
        def deserialize(cls, data):
            if len(data)>4096:
                raise ValueError("response bound")
            return bytes(data), b""
    protocol=ezsp._protocol
    protocol.COMMANDS=dict(protocol.COMMANDS)
    for name,(cid,tx,rx) in list(protocol.COMMANDS.items()):
        if cid in (0x26,0x27,0x28,0x69,0x116,0x91,0x114,0x52,0x10c,0x100,0x101,0x102,0x10f,0x4a):
            protocol.COMMANDS[name]=(cid,tx,RawResponse)
            protocol.COMMANDS_BY_ID[cid]=(name,tx,RawResponse)


async def capture(ezsp):
    from bellows.ezsp.v13.commands import COMMANDS
    import bellows.types as t
    names = {cid: name for name, (cid, _, _) in COMMANDS.items()}
    empty = b''
    queries = deque([(0x26, empty), (0x27, empty), (0x28, empty), (0x69, empty),
                     (0x116, empty), (0x91, empty), (0x114, b'\x01'+bytes(17)),
                     (0x114, b'\x02'+bytes(17)), (0x52, b'\x1e'), (0x52, b'\x11'),
                     (0x52, b'\x0d'), (0x10c, b'\x02'+bytes(17)), (0x100, empty)])
    raw = {'format':'microhad-ezsp-export', 'version':1, 'ezsp_version':13, 'records':[]}
    async def query(cid, payload):
        name = names[cid]; _, tx, rx = COMMANDS[name]
        values = []; tail = payload
        for typ in tx.values():
            value, tail = typ.deserialize(tail); values.append(value)
        if tail:
            raise ValueError('trailing request data')
        result = await asyncio.wait_for(getattr(ezsp, name)(*values), 10)
        if isinstance(result,bytes):
            response=result
        elif isinstance(rx, dict):
            response = b''.join(typ(value).serialize() for typ, value in zip(rx.values(), result))
        else:
            response = result.serialize()
        raw['records'].append({'command':cid,'request':payload.hex(),'response':response.hex()})
        return response
    while queries:
        if len(raw['records']) > 2048:
            raise ValueError('query limit')
        cid, payload = queries.popleft(); response = await query(cid, payload)
        if cid == 0x52 and payload != b'\x0d':
            if len(response)!=3 or response[0] or response[2]:
                raise ValueError('invalid table size')
            queries.extend((0x10f if payload==b'\x1e' else 0x4a, bytes([i]))
                           for i in range(response[1]))
        elif cid == 0x100:
            if len(response)!=1:
                raise ValueError('invalid token count')
            queries.extend((0x101, bytes([i])) for i in range(response[0]))
        elif cid == 0x101:
            if response==b'\xb5':
                queries = deque((c,p) for c,p in queries if c!=0x101)
            elif len(response)==9 and response[0]==0:
                queries.extend((0x102, response[1:5]+i.to_bytes(4,'little'))
                               for i in range(response[8]))
            else:
                raise ValueError('invalid token metadata')
    for cid, payload in [(0x28,empty),(0x69,empty),(0x116,empty),(0x26,empty),
                         (0x114,b'\x01'+bytes(17)),(0x114,b'\x02'+bytes(17)),
                         (0x10c,b'\x02'+bytes(17))]:
        await query(cid,payload)
    raw['complete']=True;raw['collected_at']=datetime.now(timezone.utc).isoformat()
    normalize(raw)  # validate before publishing
    return raw


async def live(args, original):
    import bellows.ezsp
    import subprocess
    import shlex
    # A device-side ownership check before connecting; fails if service/supervisor
    # maintenance has not established the exact bridge as sole UART owner.
    stopped = f"awk '$1 == \"State:\" {{if ($2 != \"T\") exit 1; found=1}} END {{if (!found) exit 1}}' /proc/{args.supervisor_pid}/status"
    subprocess.run(ssh_args(args)+[stopped],check=True,stdout=subprocess.DEVNULL,timeout=10)
    command = shlex.join([args.maintenance, 'check-owner', '/dev/ttyS1', str(args.bridge_pid)])
    subprocess.run(ssh_args(args)+[command],check=True,stdout=subprocess.DEVNULL,timeout=10)
    ezsp = bellows.ezsp.EZSP({'path':f'socket://{args.host}:{args.port}',
                            'baudrate':115200,'flow_control':'hardware'})
    try:
        await ezsp.connect(use_thread=False)
        if ezsp.ezsp_version != 13:
            raise ValueError('EZSP v13 required')
        raw_read_protocol(ezsp)
        raw = await asyncio.wait_for(capture(ezsp),120)
        compare(original,normalize(raw))
        private_write(args.output,(json.dumps(raw,indent=2)+'\n').encode())
        if args.enter_bootloader:
            status, = await asyncio.wait_for(ezsp.launchStandaloneBootloader(mode=1),10)
            if int(status):
                raise ValueError('bootloader entry refused; refresh evidence before retry')
        print('Private final capture saved'+('; bootloader entry accepted' if args.enter_bootloader else '; refresh before later cutover'))
    finally:
        await asyncio.wait_for(ezsp.disconnect(),10)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compare',required=True);p.add_argument('--output')
    p.add_argument('--host');p.add_argument('--port',type=int,default=8888)
    p.add_argument('--password-file');p.add_argument('--bridge-pid',type=int)
    p.add_argument('--supervisor-pid',type=int)
    p.add_argument('--maintenance',default='/tuya/ziggurat-stage/gateway-maintenance')
    p.add_argument('--exclusive-radio',action='store_true');p.add_argument('--capture',action='store_true')
    p.add_argument('--enter-bootloader',action='store_true')
    args=p.parse_args();original=normalize(json.loads(Path(args.compare).read_text()))
    if not args.capture:
        print('Comparison export validated offline; no radio connection');return
    if not all((args.host,args.password_file,args.bridge_pid,args.supervisor_pid,args.exclusive_radio,args.output)):
        p.error('capture requires host/password-file/bridge-pid/exclusive-radio/output')
    if args.bridge_pid<=1 or args.supervisor_pid<=1 or Path(args.output).exists() or Path(args.output).parent.stat().st_mode&0o077:
        p.error('invalid bridge PID or private output')
    logging.getLogger('bellows').setLevel(logging.CRITICAL)
    asyncio.run(asyncio.wait_for(live(args,original),160))


if __name__=='__main__':
    try:main()
    except Exception:
        raise SystemExit('Final capture failed; no handover certified')
