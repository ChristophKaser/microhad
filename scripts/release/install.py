#!/usr/bin/env python3
"""Install a verified binary bundle from a Linux host. Never flashes a radio."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
from common import sha, verify_binary

ROOT = Path(__file__).resolve().parents[2]
TARGET = '/tuya/native'


class Installer:
    def __init__(self, args):
        self.args = args
        self.env = {k: v for k, v in os.environ.items() if k in ('PATH', 'HOME', 'TMPDIR', 'LANG', 'SSH_AUTH_SOCK')}
        self.env.update(WORKSPACE_CONFIG='/dev/null', TOOLCHAIN='crosstool-ng', LINK_MODE='static',
                        SSH_PORT=str(args.ssh_port), SSH_USER='root', SSH_PASSWORD_FILE=str(args.password_file),
                        TARGET_READELF=shutil.which('readelf') or 'readelf')
        self.ssh = ['sh', str(ROOT / 'scripts/workspace-ssh.sh'), '--password-file', str(args.password_file), '--', args.host]

    def remote(self, command, data=None):
        return subprocess.check_output(self.ssh + [command], input=data, env=self.env).decode().strip()

    def preflight(self):
        # This command is deliberately read-only and is also used by check.
        result = self.remote("test -c /dev/ttyS1 && test -f /tuya/tuya_start.sh && "
            "if grep -qi mips /proc/cpuinfo; then echo mips; else echo unsupported; fi; "
            "df -k /tuya | awk 'END {print $4}'; "
            "if [ -f /tuya/native/config.json ]; then echo configured; else echo fresh; fi; "
            "if [ -f /tmp/ziggurat-supervisor/supervisor.pid ] || grep -q paired_start /tuya/tuya_start.sh; then echo paired; "
            "elif grep -q 'microhad --config' /tuya/tuya_start.sh; then echo microhad-only; else echo prepared; fi; "
            "for p in /proc/[0-9]*/exe; do x=$(ls -l \"$p\" 2>/dev/null) || continue; "
            "case \"$x\" in *' -> /tuya/native/'*' (deleted)') echo deleted;; esac; done")
        lines = result.splitlines()
        if len(lines) < 4 or not lines[0].startswith('mips') or not lines[1].isdigit():
            raise ValueError('Unsupported gateway or missing prepared launcher/UART')
        if 'deleted' in lines[4:]:
            raise ValueError('Gateway has a running deleted executable; restart its owner before installing')
        names = ('microhad-ezsp',) if self.args.mode == 'microhad-only' else ('microhad', 'mthread-matter', 'ziggurat', 'gateway-maintenance')
        candidates = [ROOT / 'bin' / name for name in names if (ROOT / 'bin' / name).is_file()]
        candidates += [p for p in (ROOT / 'webui').rglob('*') if p.is_file()]
        largest = max((p.stat().st_size for p in candidates), default=0)
        required = 512 if lines[2] == 'configured' and self.args.mode == 'paired' else (largest + 1023) // 1024 + 512
        if int(lines[1]) < required:
            raise ValueError('Insufficient flash for the largest artifact plus the 512 KiB reserve')
        self.fresh = lines[2] == 'fresh'
        mode = lines[3]
        if mode not in ('prepared', self.args.mode):
            raise ValueError('Backend change refused: follow the radio/network migration guide')
        if self.args.action == 'install' and not self.fresh:
            raise ValueError('Existing configuration found; use update instead of install')
        if self.args.action == 'update' and (self.fresh or mode == 'prepared'):
            raise ValueError('No recognized installed stack; use the prepared-device installation procedure')
        print('Preflight passed (' + ('fresh device' if self.fresh else mode) + ')')

    def upload(self, source, destination, mode='700', absent=False):
        # Used only for bootstrap/private inputs and the native-only transaction.
        # Keep the reserve even for tiny configuration files.
        temporary = destination + '.release-new'
        q = shlex.quote
        guard = 'test ! -e ' + q(destination) + ' && ' if absent else ''
        self.remote(guard + 'test "$(df -k /tuya | awk \'END {print $4}\')" -gt 512 && mkdir -p ' + q(str(Path(destination).parent)))
        self.remote('umask 077; dd bs=64k of=' + q(temporary), Path(source).read_bytes())
        expected = hashlib.md5(Path(source).read_bytes()).hexdigest()
        command = ('test "$(df -k /tuya | awk \'END {print $4}\')" -ge 512 && '
                   'test "$(md5sum ' + q(temporary) + ' | awk \'{print $1}\')" = ' + q(expected) + ' && chmod ' + mode + ' ' + q(temporary) + ' && ')
        # Hard-link creation is atomic and cannot overwrite a newly appeared identity.
        command += ('ln ' + q(temporary) + ' ' + q(destination) + ' && rm -f ' + q(temporary) if absent
                    else 'mv ' + q(temporary) + ' ' + q(destination))
        try:
            self.remote(command)
        except subprocess.CalledProcessError:
            try:
                self.remote('rm -f ' + q(temporary))
            except subprocess.CalledProcessError:
                pass
            raise

    def check_radio_free(self):
        # Bootstrap an ephemeral helper without putting its package on flash.
        name = '/tmp/microhad-release-maintenance'
        self.remote('umask 077; dd bs=64k of=' + name, (ROOT / 'bin/gateway-maintenance').read_bytes())
        expected = hashlib.md5((ROOT / 'bin/gateway-maintenance').read_bytes()).hexdigest()
        try:
            self.remote('test "$(md5sum ' + name + ' | awk \'{print $1}\')" = ' + expected +
                        ' && chmod 700 ' + name + ' && ' + name + ' check-free /dev/ttyS1')
        finally:
            self.remote('rm -f ' + name)

    def bootstrap(self):
        self.check_radio_free()
        if self.args.mode == 'paired':
            self.remote('test ! -e /tuya/native/ziggurat-state.json && test ! -e /tuya/native/matter && test ! -e /tuya/native/thread-matter')
        self.upload(self.args.config, TARGET + '/config.json', '600', absent=True)
        if self.args.mode == 'paired':
            self.upload(self.args.ziggurat_state, TARGET + '/ziggurat-state.json', '600', absent=True)
            self.upload(ROOT / 'bin/gateway-maintenance', TARGET + '/gateway-maintenance')

    def paired(self):
        command = ['sh', str(ROOT / 'deploy.sh'), '--maintenance-deploy', '--host', self.args.host,
                   '--password-file', str(self.args.password_file), '--target', TARGET,
                   '--binary', str(ROOT / 'bin/microhad'), '--ziggurat', str(ROOT / 'bin/ziggurat'),
                   '--matter-service', str(ROOT / 'bin/mthread-matter'), '--maintenance-helper', str(ROOT / 'bin/gateway-maintenance'),
                   '--webui-dir', str(ROOT / 'webui')]
        if self.args.rollback_dir:
            command += ['--rollback-dir', str(self.args.rollback_dir)]
        subprocess.run(command, env=self.env, check=True)

    def native(self):
        # Stop only the recognized native supervisor and its exact child image.
        if not self.fresh:
            self.remote("pids=; parents=; for p in /proc/[0-9]*/exe; do "
                "x=$(ls -l \"$p\" 2>/dev/null) || continue; case \"$x\" in *' -> /tuya/native/microhad') "
                "pid=${p#/proc/}; pid=${pid%/exe}; parent=$(awk '{print $4}' /proc/$pid/stat); "
                "[ \"$parent\" -gt 1 ] || exit 1; command=$(tr '\\000' ' ' < /proc/$parent/cmdline); "
                "case \"$command\" in *'/tuya/tuya_start.sh'*) ;; *) exit 1;; esac; "
                "pids=\"$pids $pid\"; parents=\"$parents $parent\";; esac; done; "
                "[ -n \"$pids\" ] || exit 1; kill -TERM $parents; kill -TERM $pids; "
                "n=0; while [ $n -lt 15 ]; do alive=; for pid in $pids $parents; do "
                "if [ -d /proc/$pid ]; then alive=1; fi; done; [ -z \"$alive\" ] && exit 0; sleep 1; n=$((n+1)); done; exit 1")
        self.check_radio_free()
        self.upload(ROOT / 'bin/microhad-ezsp', TARGET + '/microhad')
        for path in sorted((ROOT / 'webui').rglob('*'), key=lambda p: (p.name == 'index.html', str(p))):
            if path.is_file():
                self.upload(path, TARGET + '/webui/' + path.relative_to(ROOT / 'webui').as_posix(), '644')
        self.upload(ROOT / 'microhad/native_start.sh', '/tuya/tuya_start.sh', '755')
        self.remote('/tuya/tuya_start.sh </dev/null >/dev/null 2>&1')
        self.verify_dashboard()

    def backup(self):
        if not self.args.rollback_dir:
            return
        directory = self.args.rollback_dir
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if any(directory.iterdir()):
            raise ValueError('Rollback directory must be empty')
        directory.chmod(0o700)
        # Paired updates delegate the existing, broader backup to deploy.sh.
        if self.args.mode == 'paired':
            return
        for name, remote in [('microhad', TARGET + '/microhad'), ('gateway_start.sh', '/tuya/tuya_start.sh')]:
            data = subprocess.check_output(self.ssh + ['if [ -f ' + remote + ' ]; then cat ' + remote + '; fi'], env=self.env)
            if data:
                path = directory / name
                path.write_bytes(data)
                path.chmod(0o600)
                remote_hash = self.remote('md5sum ' + remote + " | awk '{print $1}'")
                if hashlib.md5(data).hexdigest() != remote_hash:
                    raise ValueError('Rollback copy checksum mismatch')
        data = subprocess.check_output(self.ssh + ['if [ -d /tuya/native/webui ]; then tar -cf - -C /tuya/native webui; fi'], env=self.env)
        if data:
            (directory / 'webui.tar').write_bytes(data)
            (directory / 'webui.tar').chmod(0o600)

    def verify_dashboard(self):
        base = 'http://' + self.args.host
        for attempt in range(30):
            running = self.remote("for p in /proc/[0-9]*/exe; do x=$(ls -l \"$p\" 2>/dev/null) || continue; "
                                  "case \"$x\" in *' -> /tuya/native/microhad') echo running; exit 0;; esac; done; echo stopped")
            if running != 'running':
                time.sleep(1)
                continue
            result = subprocess.run(['curl', '-fsS', '--connect-timeout', '2', '--max-time', '4', base + '/api/status'],
                                    capture_output=True, env=self.env)
            if result.returncode == 0:
                try:
                    status = json.loads(result.stdout)
                    if status.get('online') and status.get('coordinator_backend') == 'ezsp':
                        subprocess.run(['curl', '-fsS', '--max-time', '5', base + '/licenses.html'], stdout=subprocess.DEVNULL, env=self.env, check=True)
                        print('MicroHAD-only installation is running')
                        return
                except (ValueError, json.JSONDecodeError):
                    pass
            time.sleep(1)
        raise ValueError('Gateway did not become ready; inspect the stopped/running services before retrying')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'install', 'update'])
    parser.add_argument('--mode', choices=['paired', 'microhad-only'], default='paired')
    parser.add_argument('--host', required=True)
    parser.add_argument('--password-file', type=Path, required=True)
    parser.add_argument('--ssh-port', type=int, default=22)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--ziggurat-state', type=Path)
    parser.add_argument('--rollback-dir', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]*', args.host) or not 1 <= args.ssh_port <= 65535:
        parser.error('Provide a hostname/IPv4 address and SSH port 1..65535')
    args.password_file = args.password_file.resolve()
    if not args.password_file.is_file():
        parser.error('Password file must be readable')
    if args.action != 'install' and (args.config or args.ziggurat_state):
        parser.error('Only install accepts initial configuration/state')
    if args.action == 'install':
        if not args.config or (args.mode == 'paired' and not args.ziggurat_state):
            parser.error('install requires --config and paired mode also requires --ziggurat-state')
        config = json.loads(args.config.read_text())
        if not isinstance(config, dict):
            parser.error('Configuration must be a JSON object')
        if config.get('runtime_dir', TARGET) != TARGET:
            parser.error('Configuration runtime_dir must be /tuya/native')
        if (args.mode == 'paired') != bool(config.get('ziggurat_socket')):
            parser.error('ziggurat_socket must match the selected backend')
        if args.ziggurat_state:
            if args.mode != 'paired' or not isinstance(json.loads(args.ziggurat_state.read_text()), dict):
                parser.error('Prepared Zigbee state is only supported in paired mode')
    for executable in ('sshpass', 'ssh', 'curl', 'readelf', 'strings', 'tar'):
        if not shutil.which(executable):
            parser.error('Required host utility missing: ' + executable)
    verify_binary(ROOT)
    installer = Installer(args)
    installer.preflight()
    if args.action == 'check':
        return
    installer.backup()
    if args.action == 'install':
        installer.bootstrap()
    if args.mode == 'paired':
        installer.paired()
    else:
        installer.native()
    print('Complete. Configuration and network identities have been preserved.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        # Never echo SSH output, JSON contents or command arguments containing private paths.
        print('Installation failed (' + type(error).__name__ + '). No automatic rollback was attempted.\n'
              'Inspect service state and retained backups before retrying; do not erase network/identity files.', file=sys.stderr)
        if isinstance(error, ValueError):
            print(str(error), file=sys.stderr)
        raise SystemExit(1)
