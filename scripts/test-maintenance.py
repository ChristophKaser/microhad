#!/usr/bin/env python3
"""UART maintenance tests on host PTYs only; no physical UART opens."""
import errno
import os
from pathlib import Path
import pty
import select
import subprocess
import termios
import threading
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/gateway-maintenance-host'


class Maintenance(unittest.TestCase):
    def test_requires_explicit_flag_and_restricts_paths(self):
        self.assertEqual(subprocess.run([str(BINARY),'bootloader-enter','/dev/null']).returncode,2)
        self.assertEqual(subprocess.run([str(BINARY),'bootloader-enter','/dev/null','--exclusive-radio']).returncode,2)

    def test_existing_owner_refused_before_bytes_sent(self):
        master,slave=pty.openpty()
        try:
            result=subprocess.run([str(BINARY),'bootloader-enter',os.ttyname(slave),'--exclusive-radio'])
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(select.select([master],[],[],.05)[0])
            self.assertEqual(subprocess.run([str(BINARY),'check-owner',os.ttyname(slave),str(os.getpid())]).returncode,0)
            self.assertNotEqual(subprocess.run([str(BINARY),'check-owner',os.ttyname(slave),'999999']).returncode,0)
        finally:os.close(master);os.close(slave)

    def exchange(self,mode):
        from importlib.util import spec_from_file_location,module_from_spec
        spec=spec_from_file_location('sim',ROOT/'scripts/simulate-rcp.py');sim=module_from_spec(spec);spec.loader.exec_module(sim)
        master,slave=pty.openpty();path=os.ttyname(slave);os.close(slave)
        process=subprocess.Popen([str(BINARY),mode,path,'--exclusive-radio'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        data=b'';frame_seen=False;menu_sent=False;run_seen=False
        try:
            end=time.monotonic()+15
            while process.poll() is None and time.monotonic()<end:
                if not select.select([master],[],[],.1)[0]:continue
                try:chunk=os.read(master,4096)
                except OSError as exc:
                    if exc.errno==errno.EIO:time.sleep(.02);continue
                    raise
                data+=chunk
                if mode=='bootloader-enter' and data.startswith(b'\x7e') and data.endswith(b'\x7e'):
                    self.assertEqual(sim.decode(data[1:-1]),b'\x80\x01\x03')
                    settings=termios.tcgetattr(master)
                    self.assertEqual(settings[4],termios.B460800);self.assertTrue(settings[2]&termios.CRTSCTS)
                    frame_seen=True;data=b''
                if data==b'\n' and not menu_sent:
                    settings=termios.tcgetattr(master)
                    self.assertEqual(settings[4],termios.B115200);self.assertFalse(settings[2]&termios.CRTSCTS)
                    os.write(master,b'Gecko Bootloader v1.8.0\n1. upload gbl\n2. run\nBL >');menu_sent=True;data=b''
                if b'2\n' in data:run_seen=True
            stdout,stderr=process.communicate(timeout=2)
            self.assertEqual(process.returncode,0,stderr.decode())
            # The run bytes can arrive just as the process exits.
            if mode=='bootloader-run' and not run_seen:
                if select.select([master],[],[],.1)[0]:
                    try:run_seen=b'2\n' in os.read(master,4096)
                    except OSError:pass
            self.assertTrue(menu_sent)
            self.assertEqual(frame_seen,mode=='bootloader-enter')
            self.assertEqual(run_seen,mode=='bootloader-run')
        finally:
            if process.poll() is None:process.kill();process.wait()
            os.close(master)

    def test_reset_frame_and_baud_flow_switch(self):self.exchange('bootloader-enter')
    def test_menu_only_run_never_uploads(self):self.exchange('bootloader-run')


if __name__=='__main__':unittest.main()
