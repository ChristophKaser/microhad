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
binary_only=true
activate=false
restart=false
certificate=""
private_key=""
ca_bundle=""

usage() {
    echo "usage: $0 --password-file PATH [--config PATH | --binary-only] [--host HOST] [--user USER] [--target DIR] [--binary PATH] [--tls-certificate PATH --tls-private-key PATH] [--ca-bundle PATH] [--activate] [--restart]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config) config=${2-}; binary_only=false; shift 2 ;;
        --password-file) password_file=${2-}; shift 2 ;;
        --binary-only) binary_only=true; shift ;;
        --host) host=${2-}; shift 2 ;;
        --user) user=${2-}; shift 2 ;;
        --target) target=${2-}; shift 2 ;;
        --binary) binary=${2-}; shift 2 ;;
        --tls-certificate) certificate=${2-}; shift 2 ;;
        --tls-private-key) private_key=${2-}; shift 2 ;;
        --ca-bundle) ca_bundle=${2-}; shift 2 ;;
        --activate) activate=true; shift ;;
        --restart) restart=true; shift ;;
        *) usage ;;
    esac
done

[ "$binary_only" = true ] || { [ -n "$config" ] && [ -f "$config" ]; } || usage
[ "$binary_only" = false ] || [ -z "$config" ] || usage
[ -n "$password_file" ] && [ -r "$password_file" ] || usage
[ -x "$binary" ] || { echo "MIPS binary not found or not executable: $binary" >&2; exit 1; }
[ -z "$certificate" ] || [ -f "$certificate" ] || usage
[ -z "$private_key" ] || [ -f "$private_key" ] || usage
[ -z "$ca_bundle" ] || [ -f "$ca_bundle" ] || usage
[ -z "$certificate" ] && [ -z "$private_key" ] || { [ -n "$certificate" ] && [ -n "$private_key" ] || usage; }
command -v sshpass >/dev/null 2>&1 || { echo "sshpass is required for password authentication" >&2; exit 1; }

# Quote paths defensively: deployment targets are deliberately restricted to
# the gateway data volume, preventing an accidental upload elsewhere.
case "$target" in /tuya/*) ;; *) echo "target must be below /tuya" >&2; exit 2 ;; esac
case "$target" in *[!A-Za-z0-9_./-]*) echo "target contains unsupported characters" >&2; exit 2 ;; esac
[ "$activate" = false ] || [ "$target" = /tuya/native ] || { echo "--activate requires --target /tuya/native" >&2; exit 2; }
[ "$restart" = false ] || [ "$target" = /tuya/native ] || { echo "--restart requires --target /tuya/native" >&2; exit 2; }

ssh_command() {
    sshpass -f "$password_file" ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=yes \
        -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
        -o PreferredAuthentications=password -o PubkeyAuthentication=no "$user@$host" "$1"
}

printf 'Uploading native binary to %s:%s\n' "$host" "$target"
ssh_command "mkdir -p \"$target\"; rm -f \"$target/.microhad-zigbee-native.new\" \"$target/.config.json.new\""
ssh_command "dd of=\"$target/.microhad-zigbee-native.new\"" < "$binary"
if [ "$binary_only" = true ]; then
    ssh_command "chmod 700 \"$target/.microhad-zigbee-native.new\"; mv \"$target/.microhad-zigbee-native.new\" \"$target/tuya-zigbee-native\"; ls -l \"$target/tuya-zigbee-native\""
else
    ssh_command "dd of=\"$target/.config.json.new\"" < "$config"
    ssh_command "chmod 700 \"$target/.microhad-zigbee-native.new\"; chmod 600 \"$target/.config.json.new\"; mv \"$target/.microhad-zigbee-native.new\" \"$target/tuya-zigbee-native\"; mv \"$target/.config.json.new\" \"$target/config.json\"; ls -l \"$target/tuya-zigbee-native\" \"$target/config.json\""
fi
if [ -n "$certificate" ]; then
    ssh_command "mkdir -p \"$target/tls\"; dd of=\"$target/tls/.cert.pem.new\"" < "$certificate"
    ssh_command "dd of=\"$target/tls/.key.pem.new\"" < "$private_key"
    ssh_command "chmod 644 \"$target/tls/.cert.pem.new\"; chmod 600 \"$target/tls/.key.pem.new\"; mv \"$target/tls/.cert.pem.new\" \"$target/tls/cert.pem\"; mv \"$target/tls/.key.pem.new\" \"$target/tls/key.pem\""
fi
if [ -n "$ca_bundle" ]; then
    ssh_command "mkdir -p \"$target/tls\"; dd of=\"$target/tls/.ca-bundle.pem.new\"" < "$ca_bundle"
    ssh_command "chmod 644 \"$target/tls/.ca-bundle.pem.new\"; mv \"$target/tls/.ca-bundle.pem.new\" \"$target/tls/ca-bundle.pem\""
fi
if [ "$activate" = true ]; then
    ssh_command "dd of=\"$target/.gateway_start.sh.new\"" < "$project_dir/native_start.sh"
    ssh_command "test -f /tuya/tuya_start.sh.lowjs-backup || cp /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup; chmod 755 \"$target/.gateway_start.sh.new\"; mv \"$target/.gateway_start.sh.new\" /tuya/tuya_start.sh"
fi
if [ "$restart" = true ]; then
    printf 'Restarting the supervised native daemon\n'
    ssh_command "killall -TERM microhad-zigbee-native >/dev/null 2>&1 || exit 1; sleep 3; retries=0; while [ \$retries -lt 10 ]; do if ps w | grep -q '[t]uya-zigbee-native'; then exit 0; fi; retries=\$((retries + 1)); sleep 1; done; exit 1"
    echo "Uploaded and restarted."
elif [ "$activate" = true ]; then
    echo "Uploaded and activated for the next boot. Existing processes were not restarted."
else
    echo "Uploaded. Existing services were not changed."
fi
