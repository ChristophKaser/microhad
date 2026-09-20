#!/bin/sh
# Upload a verified MIPS binary and a caller-supplied runtime config to a
# Dropbear-only gateway gateway. The server has no SCP subsystem, so files are
# transferred through SSH stdin and moved into place only after each transfer.

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
host=192.168.178.62
user=root
target=/tuya/native-test
binary="$project_dir/build/mips/microhad-zigbee-native"
password_file=""
config=""

usage() {
    echo "usage: $0 --config PATH --password-file PATH [--host HOST] [--user USER] [--target DIR] [--binary PATH]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config) config=${2-}; shift 2 ;;
        --password-file) password_file=${2-}; shift 2 ;;
        --host) host=${2-}; shift 2 ;;
        --user) user=${2-}; shift 2 ;;
        --target) target=${2-}; shift 2 ;;
        --binary) binary=${2-}; shift 2 ;;
        *) usage ;;
    esac
done

[ -n "$config" ] && [ -f "$config" ] || usage
[ -n "$password_file" ] && [ -r "$password_file" ] || usage
[ -x "$binary" ] || { echo "MIPS binary not found or not executable: $binary" >&2; exit 1; }
command -v sshpass >/dev/null 2>&1 || { echo "sshpass is required for password authentication" >&2; exit 1; }

# Quote paths defensively: deployment targets are deliberately restricted to
# the gateway data volume, preventing an accidental upload elsewhere.
case "$target" in /tuya/*) ;; *) echo "target must be below /tuya" >&2; exit 2 ;; esac
case "$target" in *[!A-Za-z0-9_./-]*) echo "target contains unsupported characters" >&2; exit 2 ;; esac

ssh_command() {
    sshpass -f "$password_file" ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=yes \
        -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
        -o PreferredAuthentications=password -o PubkeyAuthentication=no "$user@$host" "$1"
}

printf 'Uploading native binary to %s:%s\n' "$host" "$target"
ssh_command "mkdir -p \"$target\"; rm -f \"$target/.microhad-zigbee-native.new\" \"$target/.config.json.new\""
ssh_command "dd of=\"$target/.microhad-zigbee-native.new\"" < "$binary"
ssh_command "dd of=\"$target/.config.json.new\"" < "$config"
ssh_command "chmod 700 \"$target/.microhad-zigbee-native.new\"; chmod 600 \"$target/.config.json.new\"; mv \"$target/.microhad-zigbee-native.new\" \"$target/tuya-zigbee-native\"; mv \"$target/.config.json.new\" \"$target/config.json\"; ls -l \"$target/tuya-zigbee-native\" \"$target/config.json\""
echo "Uploaded. Existing services were not changed."
