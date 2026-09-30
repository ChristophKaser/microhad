#!/bin/sh
# Upload selected native gateway assets and an optional runtime config to a
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
deploy_binary=true
deploy_webui=true
activate=false
restart=false
certificate=""
private_key=""
ca_bundle=""

usage() {
    echo "usage: $0 --password-file PATH [--binary-only | --webui-only | --both] [--config PATH] [--host HOST] [--user USER] [--target DIR] [--binary PATH] [--tls-certificate PATH --tls-private-key PATH] [--ca-bundle PATH] [--activate] [--restart]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config) config=${2-}; shift 2 ;;
        --password-file) password_file=${2-}; shift 2 ;;
        --binary-only) deploy_binary=true; deploy_webui=false; shift ;;
        --webui-only) deploy_binary=false; deploy_webui=true; shift ;;
        --both) deploy_binary=true; deploy_webui=true; shift ;;
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

[ -n "$password_file" ] && [ -r "$password_file" ] || usage
[ "$deploy_binary" = false ] || [ -x "$binary" ] || { echo "MIPS binary not found or not executable: $binary" >&2; exit 1; }
[ "$deploy_webui" = false ] || { [ -f "$project_dir/webui/dist/index.html" ] && [ -f "$project_dir/webui/dist/sw.js" ]; } || { echo "Built Web UI not found; run make webui first" >&2; exit 1; }
[ -z "$config" ] || { [ "$deploy_binary" = true ] && [ -f "$config" ]; } || usage
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
[ "$deploy_binary" = true ] || { [ -z "$config" ] && [ "$activate" = false ] && [ "$restart" = false ]; } || { echo "--config, --activate, and --restart require binary deployment" >&2; exit 2; }

ssh_command() {
    remote_command=$1
    sshpass -f "$password_file" ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=yes \
        -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
        -o PreferredAuthentications=password -o PubkeyAuthentication=no "$user@$host" "$remote_command" || {
            status=$?
            printf 'ERROR: SSH command failed for %s@%s (exit %s): %s\n' \
                "$user" "$host" "$status" "$remote_command" >&2
            return "$status"
        }
}

prune_web_bundles() {
    assets_dir="$target/webui/assets"
    ssh_command "index=\$(cat \"$target/webui/index.html\" 2>/dev/null || true); for asset in \"$assets_dir\"/index-*.js \"$assets_dir\"/index-*.css; do [ -f \"\$asset\" ] || continue; name=\${asset##*/}; case \"\$index\" in *\"\$name\"*) ;; *) rm -f \"\$asset\" ;; esac; done; rm -f \"$assets_dir\"/index-*.js.new \"$assets_dir\"/index-*.css.new"
}

if [ "$deploy_binary" = true ]; then
    printf 'Uploading native binary to %s:%s\n' "$host" "$target"
    ssh_command "mkdir -p \"$target\"; rm -f \"$target/.microhad-zigbee-native.new\" \"$target/.config.json.new\""
    ssh_command "dd of=\"$target/.microhad-zigbee-native.new\"" < "$binary"
    if [ -z "$config" ]; then
        ssh_command "chmod 700 \"$target/.microhad-zigbee-native.new\"; mv \"$target/.microhad-zigbee-native.new\" \"$target/tuya-zigbee-native\"; ls -l \"$target/tuya-zigbee-native\""
    else
        ssh_command "dd of=\"$target/.config.json.new\"" < "$config"
        ssh_command "chmod 700 \"$target/.microhad-zigbee-native.new\"; chmod 600 \"$target/.config.json.new\"; mv \"$target/.microhad-zigbee-native.new\" \"$target/tuya-zigbee-native\"; mv \"$target/.config.json.new\" \"$target/config.json\"; ls -l \"$target/tuya-zigbee-native\" \"$target/config.json\""
    fi
fi
if [ "$deploy_webui" = true ]; then
    printf 'Uploading web UI to %s:%s/webui\n' "$host" "$target"
    webui_dist="$project_dir/webui/dist"
    # Free flash before copying new assets while the active index and its
    # referenced bundles remain available if the transfer is interrupted.
    prune_web_bundles
    find "$webui_dist" -type f ! -name index.html | while IFS= read -r asset; do
        relative=${asset#"$webui_dist"/}
        case "$relative" in *[!A-Za-z0-9_./-]*|*..*) echo "unsupported Web UI asset path: $relative" >&2; exit 1 ;; esac
        asset_dir=${relative%/*}
        if [ "$asset_dir" = "$relative" ]; then asset_dir=.; fi
        # Keep SSH from consuming the remaining filenames on find's stdin.
        ssh_command "mkdir -p \"$target/webui/$asset_dir\"" < /dev/null
        ssh_command "dd of=\"$target/webui/$relative.new\"" < "$asset"
        ssh_command "chmod 644 \"$target/webui/$relative.new\"; mv \"$target/webui/$relative.new\" \"$target/webui/$relative\"" < /dev/null
    done
    ssh_command "dd of=\"$target/webui/.index.html.new\"" < "$webui_dist/index.html"
    ssh_command "chmod 644 \"$target/webui/.index.html.new\"; mv \"$target/webui/.index.html.new\" \"$target/webui/index.html\"; ls -l \"$target/webui/index.html\""
    prune_web_bundles
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
    ssh_command "if ! killall -TERM microhad-zigbee-native >/dev/null 2>&1; then echo 'ERROR: restart check failed: killall could not signal microhad-zigbee-native' >&2; exit 1; fi; sleep 3; retries=0; while [ \$retries -lt 10 ]; do if ps w | grep -q '[t]uya-zigbee-native'; then exit 0; fi; retries=\$((retries + 1)); sleep 1; done; echo 'ERROR: restart check failed: microhad-zigbee-native was not found by ps within 13 seconds' >&2; ps w >&2; exit 1"
    echo "Selected assets uploaded and service restarted."
elif [ "$activate" = true ]; then
    echo "Selected assets uploaded and activated for the next boot. Existing processes were not restarted."
else
    echo "Selected assets uploaded. Existing services were not changed."
fi
