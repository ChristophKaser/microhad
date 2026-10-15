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
 level=sys.argv[sys.argv.index('--log-level')+1]
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write('zig level '+level+'\\n')
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write('zig raw '+('on' if '--raw-frames' in sys.argv else 'off')+'\\n')
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write('zig radio-trace '+os.environ.get('MATTER_RADIO_TRACE','unset')+'\\n')
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write('zig no-color '+os.environ.get('NO_COLOR','unset')+'\\n')
 path=sys.argv[sys.argv.index('--listen')+1][5:]
 Path(path).unlink(missing_ok=True)
 s=socket.socket(socket.AF_UNIX);s.bind(path);os.chmod(path,0o600);s.listen();s.settimeout(.1)
 if '--thread-socket' in sys.argv and os.environ.get('TEST_NO_THREAD')!='1':
  path=sys.argv[sys.argv.index('--thread-socket')+1]
  Path(path).unlink(missing_ok=True)
  thread=socket.socket(socket.AF_UNIX);thread.bind(path);os.chmod(path,0o600);thread.listen()
 while True:
  try:
   c,_=s.accept();c.sendall(bytes.fromhex('01020302020100'));c.close()
  except socket.timeout:pass
else:
 if role=='matter':
  level=sys.argv[sys.argv.index('--log-level')+1]
  with open(os.environ['TEST_EVENTS'],'a') as f:f.write('matter level '+level+'\\n')
  with open(os.environ['TEST_EVENTS'],'a') as f:f.write('matter raw '+('on' if '--raw-frames' in sys.argv else 'off')+'\\n')
 if role=='native' and os.environ.get('TEST_EXIT')=='1':raise SystemExit(0)
 if role=='matter' and os.environ.get('TEST_MATTER_EXIT')=='1':raise SystemExit(1)
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
        for name in ('zig','native','matter'):
            p=self.root/name;p.write_text(FAKE);p.chmod(0o700)
        syslogd=self.root/'syslogd';syslog_state=self.root/'syslogd.running'
        syslogd.write_text('#!/bin/sh\nif [ "$1" = ps ]; then [ ! -f "'+str(syslog_state)+'" ] || '
                           'echo "999999 root 1 S /bin/busybox syslogd -C512 -L -R 192.168.178.51:7878"\n'
                           'elif [ "$1" = syslogd ]; then : > "'+str(syslog_state)+'"; fi\n')
        syslogd.chmod(0o700)
        self.control=self.root/'control';self.events=self.root/'events';self.events.touch()
        self.env=dict(os.environ,ZIGGURAT_BIN=str(self.root/'zig'),NATIVE_BIN=str(self.root/'native'),
                      NATIVE_CONFIG=str(self.root/'config.json'),MAINTENANCE_BIN=str(ROOT.parent/'build/gateway-maintenance-host'),
                      SYSLOGD_BIN=str(syslogd),
                      RUNTIME_CONTROL=str(self.control),ZIGGURAT_SOCKET=str(self.root/'api.sock'),
                      ZIGGURAT_DEVICE='/dev/forbidden-physical-radio',RESTART_LIMIT='3',RESTART_BACKOFF='1',
                      STARTUP_LIMIT='2',STOP_LIMIT='2',TEST_EVENTS=str(self.events))
        self.master,slave=pty.openpty();self.env['ZIGGURAT_DEVICE']=os.ttyname(slave);os.close(slave)
        self.process=None;self.log=tempfile.TemporaryFile()
    def start(self):
        self.process=subprocess.Popen(['sh',str(ROOT/'paired_start.sh')],env=self.env,stdout=self.log,stderr=self.log)
    def ready(self):
        wait(lambda:'native start' in self.events.read_text())
    def enable_matter(self):
        self.env.update(MATTER_BIN=str(self.root/'matter'),MATTER_STATE_DIR=str(self.root/'matter-state'),
                        MATTER_SOCKET=str(self.root/'matter.sock'),THREAD_SOCKET=str(self.root/'thread.sock'),
                        MATTER_RESTART_LIMIT='2')
    def quiesce(self):
        subprocess.run(['sh',str(ROOT/'scripts/paired_control.sh'),'quiesce'],env=self.env,check=True,stdout=subprocess.DEVNULL,timeout=15)
    def stop(self):
        subprocess.run(['sh',str(ROOT/'scripts/paired_control.sh'),'stop','--frozen-snapshot-saved'],env=self.env,check=True)
        self.process.wait(timeout=10)
    def run_control(self,*args,check=True):
        return subprocess.run(['sh',str(ROOT/'scripts/paired_control.sh'),*args],env=self.env,
                              check=check,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=10)
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
    def test_log_level_control_restarts_only_ziggurat(self):
        self.enable_matter();self.start();self.ready()
        native_pid=int((self.control/'native.pid').read_text())
        zig_pid=int((self.control/'ziggurat.pid').read_text())
        result=self.run_control('log-level','trace')
        self.assertIn('restarting Ziggurat',result.stdout)
        wait(lambda:int((self.control/'ziggurat.pid').read_text())!=zig_pid)
        wait(lambda:'zig level trace' in self.events.read_text())
        wait(lambda:'zig no-color 1' in self.events.read_text())
        wait(lambda:(self.control/'active-log-level').read_text().strip()=='trace')
        self.assertEqual(int((self.control/'native.pid').read_text()),native_pid)
        self.assertNotIn('native stop',self.events.read_text())
        matter_pid=int((self.control/'matter.pid').read_text())
        os.kill(matter_pid,signal.SIGTERM)
        wait(lambda:self.events.read_text().count('matter start')==2)
        self.assertEqual(int((self.control/'native.pid').read_text()),native_pid)
        self.quiesce();self.stop()
    def test_log_level_control_validation_and_same_level_noop(self):
        self.start();self.ready()
        zig_pid=int((self.control/'ziggurat.pid').read_text())
        self.assertNotEqual(self.run_control('log-level','verbose',check=False).returncode,0)
        self.assertFalse((self.control/'requested-log-level').exists())
        self.assertEqual(int((self.control/'ziggurat.pid').read_text()),zig_pid)
        self.assertIn('already using info',self.run_control('log-level','info').stdout)
        self.assertEqual(int((self.control/'ziggurat.pid').read_text()),zig_pid)
        self.quiesce();self.stop()

    def test_raw_frames_control_retains_level_native_and_restart_budget(self):
        # Existing accounting charges initial startup as one recovery attempt.
        self.env['RESTART_LIMIT']='2'
        self.enable_matter();self.start();self.ready()
        native_pid=int((self.control/'native.pid').read_text())
        for setting in ('on','off','on'):
            zig_pid=int((self.control/'ziggurat.pid').read_text())
            result=self.run_control('raw-frames',setting)
            self.assertIn('restarting Ziggurat',result.stdout)
            wait(lambda:int((self.control/'ziggurat.pid').read_text())!=zig_pid)
            wait(lambda:(self.control/'active-raw-frames').read_text().strip()==setting)
            self.assertEqual((self.control/'active-log-level').read_text().strip(),'info')
            self.assertEqual(int((self.control/'native.pid').read_text()),native_pid)
            self.assertEqual(self.events.read_text().split('zig raw ')[-1].splitlines()[0],setting)
        self.run_control('log-level','debug')
        wait(lambda:(self.control/'active-log-level').read_text().strip()=='debug')
        self.assertEqual((self.control/'active-raw-frames').read_text().strip(),'on')
        self.assertNotIn('native stop',self.events.read_text())
        # Diagnostic switches leave the remaining recovery attempt available.
        os.kill(int((self.control/'ziggurat.pid').read_text()),signal.SIGTERM)
        wait(lambda:self.events.read_text().count('native start')==2)
        self.assertEqual((self.control/'active-raw-frames').read_text().strip(),'on')
        self.assertEqual(self.events.read_text().split('zig raw ')[-1].splitlines()[0],'on')
        self.quiesce();self.stop()
        self.events.write_text('');self.start();self.ready()
        self.assertEqual((self.control/'active-raw-frames').read_text().strip(),'off')
        self.quiesce();self.stop()

    def test_raw_frames_validation_noop_and_old_supervisor_refusal(self):
        self.start();self.ready()
        zig_pid=int((self.control/'ziggurat.pid').read_text())
        self.assertNotEqual(self.run_control('raw-frames','trace',check=False).returncode,0)
        self.assertFalse((self.control/'requested-raw-frames').exists())
        self.assertIn('already using off',self.run_control('raw-frames','off').stdout)
        (self.control/'active-raw-frames').unlink()
        self.assertNotEqual(self.run_control('raw-frames','on',check=False).returncode,0)
        self.assertEqual(int((self.control/'ziggurat.pid').read_text()),zig_pid)
        self.assertFalse((self.control/'requested-raw-frames').exists())
        self.quiesce();self.stop()

    def test_raw_frames_startup_default_and_validation(self):
        self.env['ZIGGURAT_RAW_FRAMES']='on'
        self.start();self.ready()
        self.assertIn('zig raw on',self.events.read_text())
        self.assertEqual((self.control/'active-raw-frames').read_text().strip(),'on')
        self.quiesce()
        self.assertNotEqual(self.run_control('raw-frames','off',check=False).returncode,0)
        self.stop()
        self.env['ZIGGURAT_RAW_FRAMES']='trace'
        self.start();self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode,2)

    def test_matter_radio_trace_startup_default_and_validation(self):
        self.start();self.ready()
        self.assertIn('zig radio-trace off',self.events.read_text())
        self.assertEqual((self.control/'active-radio-trace').read_text().strip(),'off')
        self.run_control('radio-trace','on')
        wait(lambda:(self.control/'active-radio-trace').exists() and
             (self.control/'active-radio-trace').read_text().strip()=='on')
        self.assertIn('zig radio-trace on',self.events.read_text())
        self.run_control('radio-trace','off')
        wait(lambda:(self.control/'active-radio-trace').exists() and
             (self.control/'active-radio-trace').read_text().strip()=='off')
        self.assertIn('zig radio-trace off',self.events.read_text())
        self.quiesce()
        self.stop()
        self.env['MATTER_RADIO_TRACE']='on'
        self.start();self.ready()
        wait(lambda:(self.control/'active-radio-trace').exists() and
             (self.control/'active-radio-trace').read_text().strip()=='on')
        self.assertIn('zig radio-trace on',self.events.read_text())
        self.assertEqual((self.control/'active-radio-trace').read_text().strip(),'on')
        self.quiesce()
        self.stop()
        self.env['MATTER_RADIO_TRACE']='yes'
        self.start();self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode,2)
    def test_matter_log_level_switch_retention_and_budget(self):
        self.enable_matter();self.env['MATTER_RESTART_LIMIT']='1';self.start();self.ready()
        wait(lambda:'matter level info' in self.events.read_text())
        native_pid=int((self.control/'native.pid').read_text())
        zig_pid=int((self.control/'ziggurat.pid').read_text())
        matter_pid=int((self.control/'matter.pid').read_text())
        for level in ('trace','debug','off'):
            self.assertIn('restarting Matter',self.run_control('matter-log-level',level).stdout)
            wait(lambda:int((self.control/'matter.pid').read_text())!=matter_pid if (self.control/'matter.pid').exists() else False)
            wait(lambda:('matter level '+level) in self.events.read_text())
            matter_pid=int((self.control/'matter.pid').read_text())
            self.assertEqual((self.control/'active-matter-log-level').read_text().strip(),level)
            self.assertEqual(int((self.control/'native.pid').read_text()),native_pid)
            self.assertEqual(int((self.control/'ziggurat.pid').read_text()),zig_pid)
        self.assertIn('already using off',self.run_control('matter-log-level','off').stdout)
        self.assertEqual(int((self.control/'matter.pid').read_text()),matter_pid)
        self.assertNotEqual(self.run_control('matter-log-level','verbose',check=False).returncode,0)
        os.kill(matter_pid,signal.SIGTERM)
        wait(lambda:(self.control/'matter.failed').exists())
        self.assertEqual(self.events.read_text().count('matter start'),4)
        self.quiesce();self.stop()

    def test_matter_log_level_default_and_unexpected_restart(self):
        self.enable_matter();self.env['MATTER_LOG_LEVEL']='warn';self.start();self.ready()
        wait(lambda:'matter level warn' in self.events.read_text())
        self.run_control('matter-log-level','trace')
        wait(lambda:'matter level trace' in self.events.read_text())
        os.kill(int((self.control/'matter.pid').read_text()),signal.SIGTERM)
        wait(lambda:self.events.read_text().count('matter level trace')==2)
        self.quiesce()
        self.assertNotEqual(self.run_control('matter-log-level','info',check=False).returncode,0)
        self.stop()
        self.events.write_text('');self.start();self.ready()
        wait(lambda:'matter level warn' in self.events.read_text())
        self.quiesce();self.stop()

    def test_matter_log_level_unavailable_and_invalid_startup(self):
        self.assertNotEqual(self.run_control('matter-log-level','trace',check=False).returncode,0)
        self.start();self.ready()
        self.assertNotEqual(self.run_control('matter-log-level','trace',check=False).returncode,0)
        self.quiesce();self.stop()
        self.env['MATTER_LOG_LEVEL']='verbose';self.start();self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode,2)

    def test_matter_raw_frames_controls_only_matter_and_retains_level(self):
        self.enable_matter();self.env['MATTER_RESTART_LIMIT']='2';self.start();self.ready()
        wait(lambda:(self.control/'active-matter-raw-frames').exists())
        native_pid=int((self.control/'native.pid').read_text())
        zig_pid=int((self.control/'ziggurat.pid').read_text())
        for setting in ('on','off','on'):
            matter_pid=int((self.control/'matter.pid').read_text())
            self.run_control('matter-raw-frames',setting)
            wait(lambda:(self.control/'matter.pid').exists() and int((self.control/'matter.pid').read_text())!=matter_pid)
            wait(lambda:(self.control/'active-matter-raw-frames').exists() and (self.control/'active-matter-raw-frames').read_text().strip()==setting)
            self.assertEqual((self.control/'active-matter-log-level').read_text().strip(),'info')
            self.assertEqual(int((self.control/'native.pid').read_text()),native_pid)
            self.assertEqual(int((self.control/'ziggurat.pid').read_text()),zig_pid)
            wait(lambda:self.events.read_text().split('matter raw ')[-1].splitlines()[0]==setting)
        self.run_control('matter-log-level','debug')
        wait(lambda:(self.control/'active-matter-log-level').exists() and (self.control/'active-matter-log-level').read_text().strip()=='debug')
        self.assertEqual((self.control/'active-matter-raw-frames').read_text().strip(),'on')
        matter_pid=int((self.control/'matter.pid').read_text())
        self.assertIn('already using on',self.run_control('matter-raw-frames','on').stdout)
        self.assertNotEqual(self.run_control('matter-raw-frames','trace',check=False).returncode,0)
        self.assertEqual(int((self.control/'matter.pid').read_text()),matter_pid)
        # Startup plus one unexpected failure fits the untouched recovery budget.
        os.kill(matter_pid,signal.SIGTERM)
        wait(lambda:(self.control/'matter.pid').exists() and int((self.control/'matter.pid').read_text())!=matter_pid)
        wait(lambda:self.events.read_text().count('matter raw on')==4)
        self.assertEqual((self.control/'active-matter-raw-frames').read_text().strip(),'on')
        self.assertEqual((self.control/'active-matter-log-level').read_text().strip(),'debug')
        self.assertEqual(int((self.control/'native.pid').read_text()),native_pid)
        self.assertEqual(int((self.control/'ziggurat.pid').read_text()),zig_pid)
        self.quiesce()
        self.assertNotEqual(self.run_control('matter-raw-frames','off',check=False).returncode,0)
        self.stop()

    def test_matter_raw_frames_startup_and_old_supervisor_refusal(self):
        self.assertNotEqual(self.run_control('matter-raw-frames','on',check=False).returncode,0)
        self.enable_matter();self.env['MATTER_RAW_FRAMES']='on';self.start();self.ready()
        wait(lambda:(self.control/'active-matter-raw-frames').exists())
        self.assertEqual((self.control/'active-matter-raw-frames').read_text().strip(),'on')
        wait(lambda:'matter raw on' in self.events.read_text())
        matter_pid=int((self.control/'matter.pid').read_text())
        (self.control/'active-matter-raw-frames').unlink()
        self.assertNotEqual(self.run_control('matter-raw-frames','off',check=False).returncode,0)
        self.assertEqual(int((self.control/'matter.pid').read_text()),matter_pid)
        self.quiesce();self.stop()
        self.env['MATTER_RAW_FRAMES']='verbose';self.start();self.process.wait(timeout=5)
        self.assertEqual(self.process.returncode,2)

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

    def test_matter_failures_exhaust_only_independent_budget(self):
        self.enable_matter();self.env['TEST_MATTER_EXIT']='1';self.start();self.ready()
        wait(lambda:(self.control/'matter.failed').exists())
        events=self.events.read_text()
        self.assertEqual(events.count('matter start'),2)
        self.assertEqual(events.count('native start'),1)
        self.assertEqual(events.count('zig start'),1)
        self.assertNotIn('native stop',events)
        self.assertIsNone(self.process.poll())
        self.quiesce();self.stop()

    def test_matter_stops_before_native_and_radio_on_quiesce(self):
        self.enable_matter();self.start();self.ready()
        wait(lambda:'matter start' in self.events.read_text())
        self.quiesce();self.stop();events=self.events.read_text()
        self.assertLess(events.index('matter stop'),events.index('native stop'))
        self.assertLess(events.index('native stop'),events.index('zig stop'))

    def test_radio_failure_stops_and_restarts_matter_with_pair(self):
        self.enable_matter();self.start();self.ready()
        wait(lambda:'matter start' in self.events.read_text())
        os.kill(int((self.control/'ziggurat.pid').read_text()),signal.SIGKILL)
        wait(lambda:self.events.read_text().count('matter start')==2)
        events=self.events.read_text()
        self.assertLess(events.index('matter stop'),events.rindex('zig start'))
        self.assertLess(events.index('matter stop'),events.index('native stop'))
        self.quiesce();self.stop()

    def test_missing_thread_endpoint_keeps_native_available(self):
        self.enable_matter();self.env['TEST_NO_THREAD']='1';self.start();self.ready();time.sleep(1.2)
        self.assertNotIn('matter start',self.events.read_text())
        self.assertIsNone(self.process.poll())
        self.quiesce();self.stop()


if __name__=='__main__':unittest.main()
