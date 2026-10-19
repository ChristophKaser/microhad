#!/bin/sh
# SSH password authentication shared by host tools. Standard SSH args follow --.
set -eu
workspace_ssh_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$workspace_ssh_root/scripts/workspace-env.sh"
workspace_load "$workspace_ssh_root"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --password-file) SSH_PASSWORD_FILE=${2:?}; SSH_PASSWORD=; shift 2 ;;
        --user) SSH_USER=${2:?}; shift 2 ;;
        --) shift; break ;;
        *) echo 'Expected --password-file PATH, --user USER, or --' >&2; exit 2 ;;
    esac
done
[ "$#" -gt 0 ] || { echo 'SSH destination required' >&2; exit 2; }
[ -z "$SSH_PASSWORD_FILE" ] || [ -z "$SSH_PASSWORD" ] || {
    echo 'Configure SSH_PASSWORD_FILE or SSH_PASSWORD, not both' >&2; exit 2;
}
workspace_password_temp=
workspace_ssh_pid=
workspace_ssh_cleanup() {
    [ -z "$workspace_ssh_pid" ] || kill "$workspace_ssh_pid" 2>/dev/null || :
    [ -z "$workspace_password_temp" ] || rm -f -- "$workspace_password_temp"
}
trap workspace_ssh_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
if [ -n "$SSH_PASSWORD" ]; then
    umask 077
    workspace_password_temp=$(mktemp "${TMPDIR:-/tmp}/microhad-ssh.XXXXXXXX")
    printf '%s\n' "$SSH_PASSWORD" > "$workspace_password_temp"
    SSH_PASSWORD_FILE=$workspace_password_temp
fi
unset SSH_PASSWORD
[ -n "$SSH_PASSWORD_FILE" ] && [ -r "$SSH_PASSWORD_FILE" ] || {
    echo 'Set SSH_PASSWORD_FILE or SSH_PASSWORD, or supply --password-file PATH' >&2; exit 2;
}
# Background + explicit stdin lets signal traps interrupt wait and remove the
# temporary credential file, while preserving binary upload/bridge stdin.
exec 3<&0
sshpass -f "$SSH_PASSWORD_FILE" ssh -p "$SSH_PORT" -l "$SSH_USER" \
    -o "ConnectTimeout=$SSH_CONNECT_TIMEOUT" -o StrictHostKeyChecking=yes \
    -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o PreferredAuthentications=password -o PubkeyAuthentication=no "$@" <&3 3<&- &
workspace_ssh_pid=$!
exec 3<&-
workspace_ssh_status=0
wait "$workspace_ssh_pid" || workspace_ssh_status=$?
workspace_ssh_pid=
exit "$workspace_ssh_status"
