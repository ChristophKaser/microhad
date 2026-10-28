#!/usr/bin/env python3
"""Host-only check: both Spinel interfaces, radio timings and a delayed RCP reply."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='radio-timing-') as directory:
    root = Path(directory)
    shim = root/'capture.so'
    capture = root/'capture.log'
    subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',
        str(ROOT/'micro-thread-matter-daemon/tests/helpers/syslog-capture.c'),'-o',str(shim)],check=True)
    env = dict(os.environ, LD_PRELOAD=str(shim), MATTER_SYSLOG_CAPTURE=str(capture))
    subprocess.run(['python3',str(ROOT/'ziggurat/scripts/test-hdlc-multipan.py'),'--debug-packets'],
                   env=env,cwd=ROOT,check=True,timeout=90)
    text = capture.read_text()
    for direction in ('RX','TX'):
        for iid in (1,2):
            assert re.search(r'SPINEL_'+direction+r' unix_us=\d+ iid='+str(iid),text), (direction,iid)
    for marker in ('SPINEL_WRITE','lock_us=','write_us=','SPINEL_REPLY','reply_us=',
                   'THREAD_REQUEST','arbitration_us=','reply_write_us='):
        assert marker in text, marker
    times = [int(value) for value in re.findall(r'reply_us=(\d+)',text)]
    assert max(times) >= 40_000, max(times)
    assert 'dropped ' not in text, 'diagnostic fixture lost log entries'
    print('PASS both Spinel packet interfaces, writer/arbitration/reply timings, and injected 50 ms radio delay')
