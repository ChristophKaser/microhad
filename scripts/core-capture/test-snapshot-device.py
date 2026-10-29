#!/usr/bin/env python3
"""Isolated userspace ELF capture gate. Requires explicit gateway selection."""
import argparse
import json
from pathlib import Path
import secrets
import shlex
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
from toolchain_config import settings


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--receiver',required=True);p.add_argument('--port',type=int,default=9999)
    p.add_argument('--quick',action='store_true',help='Skip already-verified collector fault-injection waits')
    a=p.parse_args();s=settings();out=ROOT/'build'/s['TARGET_BUILD_SUBDIR'];out.mkdir(parents=True,exist_ok=True)
    cc=[str(s['TARGET_CC']),'-std=gnu99','-Wall','-Wextra','-Werror','-static','-msoft-float']
    source=ROOT/'scripts/core-capture'
    subprocess.run(cc+['-Os','-g',str(source/'snapshot-daemon.c'),str(source/'snapshot-writer.c'),'-o',str(out/'gateway-snapshot')],check=True)
    subprocess.run(cc+['-O0','-g','-DCRASH_SNAPSHOTS_BUILD=1',str(source/'snapshot-demo.c'),str(source/'snapshot-client.c'),str(source/'snapshot-writer.c'),str(ROOT/'ziggurat/stock-uclibc-compat/native/fault_report.c'),'-Wl,--wrap=pthread_create','-lpthread','-o',str(out/'snapshot-demo')],check=True)
    subprocess.run(cc+['-Os','-g','-DSNAPSHOT_TESTING',str(source/'snapshot-daemon.c'),str(source/'snapshot-writer.c'),'-o',str(out/'gateway-snapshot-test')],check=True)
    ssh=['bash',str(ROOT/'scripts/workspace-ssh.sh'),'--',a.host]
    def run(cmd,**kw):return subprocess.run(ssh+[cmd],check=True,**kw)
    remote='/tmp/snapshot-gate.'+secrets.token_hex(8)
    run(f'umask 077; test ! -e /tmp/gateway-snapshots/notify; mkdir {remote}')
    try:
        for name in ('gateway-snapshot','gateway-snapshot-test','snapshot-demo','gateway-core'):
            with (out/name).open('rb') as f:run(f'cat > {remote}/{name}; chmod 700 {remote}/{name}',stdin=f)
        run(f"printf '%s\\n' {shlex.quote(a.receiver+' '+str(a.port))} > {remote}/core-capture.conf; {remote}/gateway-snapshot {remote}/gateway-core </dev/null >/dev/null 2>{remote}/collector.log & echo $! > {remote}/collector.pid")
        time.sleep(.5)
        output=ROOT/'diagnostics/core-dumps'
        for mode in ('main','worker','large'):
            before=set(output.glob('*/capture.json'))
            started=time.monotonic()
            environment='SNAPSHOT_DEMO_BYTES=8388608 ' if mode=='large' else ''
            argument='main' if mode=='large' else mode
            run(f'ulimit -c 0; {environment}{remote}/snapshot-demo {argument} || true',timeout=40)
            elapsed=time.monotonic()-started
            captures=set(output.glob('*/capture.json'))-before
            found=False
            for meta in captures:
                process=meta.parent/'process.txt'
                if not process.exists() or remote+'/snapshot-demo' not in process.read_text():continue
                m=json.loads(meta.read_text());assert m['complete'],m
                command=[str(s['TARGET_TOOLCHAIN_ROOT']/'bin'/(s['TARGET_PREFIX']+'gdb')),'-batch',str(out/'snapshot-demo'),str(meta.parent/'core'),'-ex','info threads','-ex','p/x demo_heap_marker','-ex','p/x demo_stack_marker','-ex','thread apply all bt']
                result=subprocess.run(command,check=True,capture_output=True,text=True)
                assert 'crash_here' in result.stdout and '0x12345678' in result.stdout and '0xabcdef12' in result.stdout,result.stdout+result.stderr
                assert result.stdout.count('LWP')>=3,result.stdout
                print('PASS',mode,m['bytes'],'bytes',round(elapsed,2),'seconds',flush=True);print(result.stdout,flush=True)
                (meta.parent/'snapshot-demo.elf').write_bytes((out/'snapshot-demo').read_bytes());found=True
            assert found,'No snapshot received'
        # Freeze network output with an unreachable receiver to exercise cleanup.
        run(f"printf '%s\\n' '127.0.0.1 1' > {remote}/core-capture.conf; {remote}/snapshot-demo worker || true",timeout=40)
        run(f'kill $(cat {remote}/collector.pid); sleep 1; cat {remote}/collector.log')
        for mode in (() if a.quick else ('DIE','PAUSE')):
            run(f'SNAPSHOT_TEST_{mode}=1 {remote}/gateway-snapshot-test {remote}/gateway-core </dev/null >/dev/null 2>{remote}/collector.log & echo $! > {remote}/collector.pid')
            time.sleep(.5)
            started=time.monotonic()
            run(f'{remote}/snapshot-demo worker || true',timeout=130)
            elapsed=time.monotonic()-started
            assert elapsed<125,elapsed
            run(f'kill $(cat {remote}/collector.pid); sleep 1')
            print('PASS collector',mode,'recovery',round(elapsed,1),'seconds',flush=True)
    finally:
        run(f'if [ -f {remote}/collector.pid ]; then kill $(cat {remote}/collector.pid) 2>/dev/null || true; fi; rm -rf {remote}')


if __name__=='__main__':main()
