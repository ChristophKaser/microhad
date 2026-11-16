"""Exercise the native update transaction against a private fake gateway."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeDeployTests(unittest.TestCase):
    def run_update(self, failure=''):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            native = root/'native'; native.mkdir()
            control = root/'control'; control.mkdir()
            proc = root/'proc'; proc.mkdir()
            for name, pid in [('native', 200), ('ziggurat', 400), ('matter', 500)]:
                (control/(name+'.pid')).write_text(str(pid)+'\n')
            for pid in (200, 201):
                (proc/str(pid)).mkdir()
                (proc/str(pid)/'exe').symlink_to(native/'microhad')
            (native/'microhad').write_bytes(b'old executable')
            (native/'microhad.matter-manifest').write_bytes(b'old manifest')
            binary = root/'binary'; binary.write_bytes(b'new executable')
            Path(str(binary)+'.matter-manifest').write_bytes(b'new manifest')
            fake = root/'ssh.py'
            fake.write_text('''import os,sys,subprocess
from pathlib import Path
r=Path(os.environ['FAKE_GATEWAY'])
command=sys.argv[1]
with (r/'commands').open('a') as f:f.write(command+'\\n')
mode=os.environ.get('FAILURE','')
if mode=='transfer' and 'dd bs=64k' in command and '.manifest-' in command:sys.exit(1)
if mode=='hash' and 'trap ' in command:
 for p in (r/'native').glob('.microhad.native-*.new'):p.write_bytes(b'bad transfer')
if mode=='reserve' and 'trap ' in command:
 command=command.replace('test "$available" -ge 512','test 0 -ge 512')
command=command.replace('/tmp/ziggurat-supervisor',str(r/'control')).replace('/proc/',str(r/'proc')+'/').replace('/tuya',str(r))
# Shell function shadows kill: no host process is ever signalled.
prelude='kill() { if [ "$1" = -TERM ]; then [ "$2" = 200 ] || exit 91; echo TERM:$2 >> "'+str(r)+'/signals"; echo 201 > "'+str(r)+'/control/native.pid"; fi; return 0; }; '
sys.exit(subprocess.run(['/bin/sh','-c',prelude+command]).returncode)
''')
            text = (ROOT/'deploy.sh').read_text()
            start = text.index("    if [ \"$changed_services\" = ' native' ]; then")
            end = text.index('    supervisor_pid=$(ssh_command', start)
            branch = text[start:end]
            runner = root/'run.sh'
            runner.write_text('''set -eu
ssh_command() { python3 "$FAKE_GATEWAY/ssh.py" "$1"; }
wait_for_dashboard() { :; }
target="$FAKE_GATEWAY/native"
binary="$FAKE_GATEWAY/binary"
changed_services=' native'
flash_reserve_kib=512
update() {
'''+branch+'\n}\nupdate\n')
            result = subprocess.run(['sh', str(runner)], env=dict(os.environ, FAKE_GATEWAY=str(root), FAILURE=failure), capture_output=True, text=True)
            self.assertEqual(result.returncode == 0, not failure, result.stdout+result.stderr)
            commands = (root/'commands').read_text()
            self.assertNotIn('kill -TERM $supervisor_pid', commands)
            self.assertNotIn(': > /tmp/ziggurat-supervisor/quiesce', commands)
            self.assertEqual((control/'matter.pid').read_text(), '500\n')
            self.assertEqual((control/'ziggurat.pid').read_text(), '400\n')
            if failure:
                self.assertFalse((root/'signals').exists())
                self.assertEqual((native/'microhad').read_bytes(), b'old executable')
                self.assertEqual((native/'microhad.matter-manifest').read_bytes(), b'old manifest')
            else:
                self.assertEqual((root/'signals').read_text(), 'TERM:200\n')
                self.assertEqual((native/'microhad').read_bytes(), b'new executable')
                self.assertEqual((native/'microhad.matter-manifest').read_bytes(), b'new manifest')

    def test_native_update_preserves_radio_and_matter(self): self.run_update()
    def test_failed_manifest_transfer_keeps_running_binary(self): self.run_update('transfer')
    def test_hash_mismatch_keeps_running_binary(self): self.run_update('hash')
    def test_flash_reserve_failure_keeps_running_binary(self): self.run_update('reserve')


if __name__ == '__main__': unittest.main()
