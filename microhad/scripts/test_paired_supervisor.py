#!/usr/bin/env python3
"""Host-only paired supervision tests with fake children and real Unix Hello probe."""
import json
import os
import pty
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
FAKE='''#!/usr/bin/env python3
import os,signal,socket,sys,time
from pathlib import Path
role=Path(sys.argv[0]).name
with open(os.environ['TEST_EVENTS'],'a') as f:f.write(role+' start '+str(os.getpid())+'\\n')
def stop(*args):
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write(role+' stop\\n')
 raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
if role=='zig':
 path=sys.argv[sys.argv.index('--listen')+1][5:]
 Path(path).unlink(missing_ok=True)
 s=socket.socket(socket.AF_UNIX);s.bind(path);os.chmod(path,0o600);s.listen();s.settimeout(.1)
 while True:
  try:
   c,_=s.accept();c.sendall(bytes.fromhex('01020302020100'));c.close()
  except socket.timeout:pass
else:
 if os.environ.get('TEST_EXIT')=='1':raise SystemExit(0)
 while True:time.sleep(.1)
'''


def wait(check,timeout=12):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if check():return
        time.sleep(.03)
    raise AssertionError('Supervisor condition deadline')


class Paired(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        for name in ('zig','native'):
            p=self.root/name;p.write_text(FAKE);p.chmod(0o700)
        self.control=self.root/'control';self.events=self.root/'events';self.events.touch()
        self.env=dict(os.environ,ZIGGURAT_BIN=str(self.root/'zig'),NATIVE_BIN=str(self.root/'native'),
                      NATIVE_CONFIG=str(self.root/'config.json'),MAINTENANCE_BIN=str(ROOT.parent/'ziggurat-compilation/build/gateway-maintenance-host'),
                      RUNTIME_CONTROL=str(self.control),ZIGGURAT_SOCKET=str(self.root/'api.sock'),
                      ZIGGURAT_DEVICE='/dev/forbidden-physical-radio',RESTART_LIMIT='3',RESTART_BACKOFF='1',
                      STARTUP_LIMIT='2',STOP_LIMIT='2',TEST_EVENTS=str(self.events))
        self.master,slave=pty.openpty();self.env['ZIGGURAT_DEVICE']=os.ttyname(slave);os.close(slave)
        self.process=None;self.log=tempfile.TemporaryFile()
    def start(self):
        self.process=subprocess.Popen(['sh',str(ROOT/'paired_start.sh')],env=self.env,stdout=self.log,stderr=self.log)
    def ready(self):
        wait(lambda:'native start' in self.events.read_text())
    def quiesce(self):
        subprocess.run(['sh',str(ROOT/'scripts/paired_control.sh'),'quiesce'],env=self.env,check=True,stdout=subprocess.DEVNULL,timeout=15)
    def stop(self):
        subprocess.run(['sh',str(ROOT/'scripts/paired_control.sh'),'stop','--frozen-snapshot-saved'],env=self.env,check=True)
        self.process.wait(timeout=10)
    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=12)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
        self.log.close();os.close(self.master);self.tmp.cleanup()
    def test_order_quiesce_retains_radio_owner_and_stop_order(self):
        self.start();self.ready();self.assertTrue(self.events.read_text().startswith('zig start'))
        self.quiesce();zig=int((self.control/'ziggurat.pid').read_text());os.kill(zig,0)
        count=self.events.read_text().count('native start');time.sleep(1.2)
        self.assertEqual(self.events.read_text().count('native start'),count)
        self.stop();events=self.events.read_text()
        self.assertLess(events.index('native stop'),events.index('zig stop'))
        self.assertFalse(self.control.exists())
    def test_existing_uart_owner_prevents_any_child_start(self):
        owner=os.open(self.env['ZIGGURAT_DEVICE'],os.O_RDWR|os.O_NOCTTY)
        try:
            self.env['RESTART_LIMIT']='1';self.start();self.process.wait(timeout=10)
            self.assertEqual(self.events.read_text(),'')
            self.assertNotEqual(self.process.returncode,0)
        finally:os.close(owner)

    def test_duplicate_supervisor_and_unguarded_stop_refused(self):
        self.start();self.ready()
        result=subprocess.run(['sh',str(ROOT/'paired_start.sh')],env=self.env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.assertNotEqual(result.returncode,0)
        result=subprocess.run(['sh',str(ROOT/'scripts/paired_control.sh'),'stop'],env=self.env,stderr=subprocess.DEVNULL)
        self.assertNotEqual(result.returncode,0)
        self.quiesce();self.stop()
    def test_ziggurat_failure_stops_native_then_restarts_pair(self):
        self.start();self.ready();zig=int((self.control/'ziggurat.pid').read_text());os.kill(zig,signal.SIGKILL)
        wait(lambda:self.events.read_text().count('native start')==2)
        events=self.events.read_text();self.assertLess(events.index('native stop'),events.rindex('zig start'))
        self.quiesce();self.stop()
    def test_native_crash_restarts_without_second_radio_owner_and_budget_bounds(self):
        self.env['TEST_EXIT']='1';self.start();self.process.wait(timeout=12)
        self.assertEqual(self.events.read_text().count('zig start'),1)
        self.assertEqual(self.events.read_text().count('native start'),3)
        self.assertNotEqual(self.process.returncode,0)
    def test_stale_socket_reclaimed_by_server_and_failed_readiness_never_starts_native(self):
        import socket
        with socket.socket(socket.AF_UNIX) as stream:stream.bind(str(self.root/'api.sock'))
        self.start();self.ready();self.quiesce();self.stop()
        self.events.write_text('');(self.root/'zig').write_text('#!/bin/sh\nexec sleep 20\n')
        self.env['RESTART_LIMIT']='1';self.start();self.process.wait(timeout=10)
        self.assertNotIn('native start',self.events.read_text())
        self.assertNotEqual(self.process.returncode,0)


if __name__=='__main__':unittest.main()
