"""Bridge to sourced, trusted development-host settings; no dotenv parser."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def settings():
    result = subprocess.run(['sh', str(ROOT / 'scripts/workspace-env.sh'), '--null'],
                            capture_output=True)
    if result.returncode:
        # A shell syntax error can include the offending credential assignment.
        raise ValueError('Cannot load workspace settings (check env-file syntax and TOOLCHAIN/SSH settings)')
    fields = result.stdout.decode().split('\0')
    return dict(zip(fields[0:-1:2], fields[1:-1:2]))


def setting(key):
    return settings()[key]


def has_credentials(password_file=None):
    values = settings()
    return bool(password_file or values['SSH_PASSWORD_FILE'] or values['SSH_PASSWORD_SET'])


def ssh_prefix(password_file=None, user=None):
    """SSH options/destination follow this prefix; credentials stay out of argv."""
    command = ['sh', str(ROOT / 'scripts/workspace-ssh.sh')]
    if password_file is not None:
        command += ['--password-file', str(password_file)]
    if user is not None:
        command += ['--user', user]
    return command + ['--']


def ssh_command(host, password_file=None, options=()):
    if not host:
        raise ValueError('Set GATEWAY_IP or supply an explicit gateway host')
    return ssh_prefix(password_file) + list(options) + [host]


def gateway_host(host):
    if not host or host.startswith('-') or '@' in host or any(c.isspace() for c in host):
        raise ValueError('Set GATEWAY_IP or provide a gateway hostname/address')
    return host
