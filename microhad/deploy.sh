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
binary_explicit=false
password_file=""
config=""
deploy_binary=true
deploy_webui=true
activate=false
activate_paired=false
activate_paired_only=false
restart=false
certificate=""
private_key=""
ca_bundle=""
matter_service=""
rng_seed_helper=""
flash_reserve_kib=${DEPLOY_FLASH_RESERVE_KIB:-512}

usage() {
    echo "usage: $0 --password-file PATH [--binary-only | --webui-only | --both | --rng-seed-only | --activate-paired-only] [--rng-seed-helper PATH] [--config PATH] [--host HOST] [--user USER] [--target DIR] [--binary PATH] [--matter-service PATH] [--tls-certificate PATH --tls-private-key PATH] [--ca-bundle PATH] [--activate | --activate-paired] [--restart]" >&2
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
        --binary) binary=${2-}; binary_explicit=true; shift 2 ;;
        --rng-seed-only) deploy_binary=false; deploy_webui=false; shift ;;
        --rng-seed-helper) rng_seed_helper=${2-}; shift 2 ;;
        --matter-service) matter_service=${2-}; shift 2 ;;
        --tls-certificate) certificate=${2-}; shift 2 ;;
        --tls-private-key) private_key=${2-}; shift 2 ;;
        --ca-bundle) ca_bundle=${2-}; shift 2 ;;
        --activate) activate=true; shift ;;
        --activate-paired) activate_paired=true; shift ;;
        --activate-paired-only) activate_paired_only=true; deploy_binary=false; deploy_webui=false; shift ;;
        --restart) restart=true; shift ;;
        *) usage ;;
    esac
done

[ -n "$password_file" ] && [ -r "$password_file" ] || usage
[ "$deploy_binary" = true ] || [ "$deploy_webui" = true ] || [ -n "$rng_seed_helper" ] || [ "$activate_paired_only" = true ] || usage
[ -z "$matter_service" ] || [ "$binary_explicit" = true ] || {
    echo "--matter-service requires --binary from a ZIGGURAT=1 MATTER=1 build" >&2; exit 2;
}
[ -z "$matter_service" ] || {
    manifest="$binary.matter-manifest"
    [ -f "$manifest" ] || { echo "Matter build manifest missing: $manifest" >&2; exit 2; }
    grep -qx 'format=microhad-native-build-v1' "$manifest" &&
        grep -qx 'ziggurat=1' "$manifest" && grep -qx 'matter=1' "$manifest" || {
            echo "Matter build manifest does not identify an enabled Ziggurat Matter binary" >&2; exit 2;
        }
    expected_hash=$(sed -n 's/^binary_sha256=//p' "$manifest")
    actual_hash=$(sha256sum "$binary" | awk '{print $1}')
    [ -n "$expected_hash" ] && [ "$expected_hash" = "$actual_hash" ] || {
        echo "Matter build manifest does not match the selected native binary" >&2; exit 2;
    }
}
[ "$deploy_binary" = false ] || [ -x "$binary" ] || { echo "MIPS binary not found or not executable: $binary" >&2; exit 1; }
[ "$deploy_webui" = false ] || { [ -f "$project_dir/webui/dist/index.html" ] && [ -f "$project_dir/webui/dist/sw.js" ]; } || { echo "Built Web UI not found; run make webui first" >&2; exit 1; }
[ -z "$rng_seed_helper" ] || [ -x "$rng_seed_helper" ] || { echo "RNG seed helper missing or not executable: $rng_seed_helper" >&2; exit 1; }
[ -z "$rng_seed_helper" ] || [ "$target" = /tuya/native ] || { echo "RNG seed helper deployment requires --target /tuya/native" >&2; exit 2; }
[ -z "$rng_seed_helper" ] || [ "$deploy_binary" = false ] || { echo "Use --rng-seed-only to deploy the RNG helper without replacing application assets" >&2; exit 2; }
[ -z "$config" ] || { [ "$deploy_binary" = true ] && [ -f "$config" ]; } || usage
[ -z "$matter_service" ] || { { [ "$deploy_binary" = true ] || [ "$activate_paired_only" = true ]; } && [ -x "$matter_service" ]; } || usage
[ -z "$certificate" ] || [ -f "$certificate" ] || usage
[ -z "$private_key" ] || [ -f "$private_key" ] || usage
[ -z "$ca_bundle" ] || [ -f "$ca_bundle" ] || usage
[ -z "$certificate" ] && [ -z "$private_key" ] || { [ -n "$certificate" ] && [ -n "$private_key" ] || usage; }
[ "$flash_reserve_kib" -ge 0 ] 2>/dev/null || {
    echo "DEPLOY_FLASH_RESERVE_KIB must be a non-negative integer" >&2; exit 2;
}
command -v sshpass >/dev/null 2>&1 || { echo "sshpass is required for password authentication" >&2; exit 1; }

# Quote paths defensively: deployment targets are deliberately restricted to
# the gateway data volume, preventing an accidental upload elsewhere.
case "$target" in /tuya/*) ;; *) echo "target must be below /tuya" >&2; exit 2 ;; esac
case "$target" in *[!A-Za-z0-9_./-]*) echo "target contains unsupported characters" >&2; exit 2 ;; esac
[ "$activate" = false ] || [ "$target" = /tuya/native ] || { echo "--activate requires --target /tuya/native" >&2; exit 2; }
[ "$activate_paired" = false ] || [ "$activate" = false ] || usage
[ "$activate_paired" = false ] || [ "$restart" = false ] || { echo "--activate-paired cannot be combined with --restart" >&2; exit 2; }
[ "$activate_paired" = false ] || [ "$target" = /tuya/native ] || { echo "--activate-paired requires --target /tuya/native" >&2; exit 2; }
[ "$activate_paired" = false ] || [ -n "$matter_service" ] || { echo "--activate-paired requires --matter-service PATH" >&2; exit 2; }
[ "$activate_paired_only" = false ] || { [ "$target" = /tuya/native ] && [ "$activate_paired" = false ] && [ "$activate" = false ] && [ "$restart" = false ] && [ -z "$config" ] && [ -n "$matter_service" ] && [ "$binary_explicit" = true ] && [ -z "$rng_seed_helper" ] && [ -z "$certificate" ] && [ -z "$private_key" ] && [ -z "$ca_bundle" ]; } || { echo "--activate-paired-only requires --target /tuya/native, --binary, and --matter-service; it cannot be combined with uploads or restart" >&2; exit 2; }
[ "$restart" = false ] || [ "$target" = /tuya/native ] || { echo "--restart requires --target /tuya/native" >&2; exit 2; }
[ "$deploy_binary" = true ] || { [ -z "$config" ] && [ "$activate" = false ] && [ "$activate_paired" = false ] && [ "$restart" = false ]; } || { echo "--config, launcher activation, and --restart require binary deployment" >&2; exit 2; }

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

matter_flash_preflight() {
    [ "$activate_paired_only" = false ] || return 0
    [ -z "$matter_service" ] && return 0

    old_sizes=$(ssh_command "for file in \"$target/tuya-zigbee-native\" \"$target/config.json\" \"$target/tuya-zigbee-native.matter-manifest\" \"$target/thread-matter\" \"$target/paired_start.sh\"; do if [ -f \"\$file\" ]; then wc -c < \"\$file\"; else echo 0; fi; done") || return $?
    set -- $old_sizes
    old_native=${1:-0}
    old_config=${2:-0}
    old_manifest=${3:-0}
    old_service=${4:-0}
    old_supervisor=${5:-0}
    for old_size in "$old_native" "$old_config" "$old_manifest" "$old_service" "$old_supervisor"; do
        case "$old_size" in ''|*[!0-9]*) echo "Unable to read existing Matter deployment file sizes" >&2; return 1 ;; esac
    done

    committed_delta=0
    flash_peak_bytes=0
    stage_flash_group() {
        stage_bytes=$1
        replaced_bytes=$2
        peak_candidate=$((committed_delta + stage_bytes))
        [ "$peak_candidate" -le "$flash_peak_bytes" ] || flash_peak_bytes=$peak_candidate
        committed_delta=$((committed_delta + stage_bytes - replaced_bytes))
    }

    binary_size=$(wc -c < "$binary")
    config_size=0
    replaced_native_and_config=$old_native
    if [ -n "$config" ]; then
        config_size=$(wc -c < "$config")
        replaced_native_and_config=$((old_native + old_config))
    fi
    stage_flash_group "$((binary_size + config_size))" "$replaced_native_and_config"
    manifest_size=$(wc -c < "$binary.matter-manifest")
    stage_flash_group "$manifest_size" "$old_manifest"
    service_size=$(wc -c < "$matter_service")
    stage_flash_group "$service_size" "$old_service"
    supervisor_size=$(wc -c < "$project_dir/paired_start.sh")
    stage_flash_group "$supervisor_size" "$old_supervisor"

    if [ "$deploy_webui" = true ]; then
        set -- $(find "$project_dir/webui/dist" -type f -exec wc -c {} + |
            awk '$2 == "total" { next } { sum += $1 } END { print sum + 0 }')
        stage_flash_group "${1:-0}" 0
    fi
    if [ -n "$certificate" ]; then
        cert_size=$(wc -c < "$certificate")
        key_size=$(wc -c < "$private_key")
        stage_flash_group "$((cert_size + key_size))" 0
    fi
    [ -z "$ca_bundle" ] || stage_flash_group "$(wc -c < "$ca_bundle")" 0
    if [ "$activate_paired" = true ]; then
        stage_flash_group "$(wc -c < "$project_dir/ziggurat_start.sh")" 0
    elif [ "$activate" = true ]; then
        stage_flash_group "$(wc -c < "$project_dir/native_start.sh")" 0
    fi
    [ "$flash_peak_bytes" -ge 0 ] || flash_peak_bytes=0
    required_kib=$(((flash_peak_bytes + 1023) / 1024 + flash_reserve_kib))
    available_kib=$(ssh_command "df -k /tuya | awk 'END {print \$4}'") || return $?
    case "$available_kib" in
        ''|*[!0-9]*) echo "Unable to read available /tuya flash space from df" >&2; return 1 ;;
    esac
    if [ "$available_kib" -lt "$required_kib" ]; then
        printf 'Insufficient /tuya flash for Matter staging: need at least %s KiB (uploads plus %s KiB reserve), have %s KiB\n' \
            "$required_kib" "$flash_reserve_kib" "$available_kib" >&2
        return 1
    fi
    printf 'Matter flash preflight passed: need at least %s KiB, have %s KiB available\n' \
        "$required_kib" "$available_kib"
}

matter_flash_preflight

paired_launcher_preflight() {
    [ "$activate_paired_only" = true ] || return 0

    ssh_command "test -x \"$target/tuya-zigbee-native\" && test -x \"$target/thread-matter\" && test -x \"$target/paired_start.sh\" && grep -qx 'format=microhad-native-build-v1' \"$target/tuya-zigbee-native.matter-manifest\" && grep -qx 'ziggurat=1' \"$target/tuya-zigbee-native.matter-manifest\" && grep -qx 'matter=1' \"$target/tuya-zigbee-native.matter-manifest\"" || {
        echo "Staged Matter package is incomplete or not executable" >&2
        return 1
    }
    expected_native_md5=$(md5sum "$binary" | awk '{print $1}')
    expected_manifest_md5=$(md5sum "$binary.matter-manifest" | awk '{print $1}')
    expected_service_md5=$(md5sum "$matter_service" | awk '{print $1}')
    expected_supervisor_md5=$(md5sum "$project_dir/paired_start.sh" | awk '{print $1}')
    actual_matter_md5s=$(ssh_command "md5sum \"$target/tuya-zigbee-native\" \"$target/tuya-zigbee-native.matter-manifest\" \"$target/thread-matter\" \"$target/paired_start.sh\"") || return $?
    set -- $actual_matter_md5s
    [ "${1:-}" = "$expected_native_md5" ] &&
        [ "${2:-}" = "$expected_manifest_md5" ] &&
        [ "${3:-}" = "$expected_service_md5" ] &&
        [ "${4:-}" = "$expected_supervisor_md5" ] || {
            echo "Staged Matter package hashes do not match the selected local artifacts" >&2
            return 1
        }
    old_launcher_sizes=$(ssh_command "for file in /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup; do if [ -f \"\$file\" ]; then wc -c < \"\$file\"; else echo 0; fi; done") || return $?
    set -- $old_launcher_sizes
    old_launcher=${1:-0}
    old_backup=${2:-0}
    case "$old_launcher:$old_backup" in *[!0-9:]*|:*|*::*) echo "Unable to read installed launcher sizes" >&2; return 1 ;; esac
    new_launcher=$(wc -c < "$project_dir/ziggurat_start.sh")
    backup_bytes=0
    [ "$old_backup" -ne 0 ] || backup_bytes=$old_launcher
    required_kib=$(((new_launcher + backup_bytes + 1023) / 1024 + flash_reserve_kib))
    available_kib=$(ssh_command "df -k /tuya | awk 'END {print \$4}'") || return $?
    case "$available_kib" in ''|*[!0-9]*) echo "Unable to read available /tuya flash space from df" >&2; return 1 ;; esac
    if [ "$available_kib" -lt "$required_kib" ]; then
        printf 'Insufficient /tuya flash for paired launcher activation: need at least %s KiB, have %s KiB\n' "$required_kib" "$available_kib" >&2
        return 1
    fi
    printf 'Paired launcher preflight passed: need at least %s KiB, have %s KiB available\n' "$required_kib" "$available_kib"
}

paired_launcher_preflight

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
if [ -n "$rng_seed_helper" ]; then
    printf 'Uploading RNG seed helper and startup supervisor to %s:%s\n' "$host" "$target"
    ssh_command "mkdir -p \"$target\"; [ ! -x \"$target/rng-seed\" ] || cp \"$target/rng-seed\" /tmp/rng-seed.pre-codex; [ ! -f \"$target/paired_start.sh\" ] || cp \"$target/paired_start.sh\" /tmp/paired_start.sh.pre-rng-seed"
    ssh_command "dd of=\"$target/.rng-seed.new\"" < "$rng_seed_helper"
    ssh_command "dd of=\"$target/.paired_start.sh.new\"" < "$project_dir/paired_start.sh"
    ssh_command "chmod 700 \"$target/.rng-seed.new\" \"$target/.paired_start.sh.new\"; mv \"$target/.rng-seed.new\" \"$target/rng-seed\"; mv \"$target/.paired_start.sh.new\" \"$target/paired_start.sh\"; ls -l \"$target/rng-seed\" \"$target/paired_start.sh\""
fi
if [ -n "$matter_service" ]; then
    # Stage the optional service and supervisor only. No launcher activation,
    # credential state transfer, UART ownership change or process restart here.
    ssh_command "dd of=\"$target/.microhad-zigbee-native.matter-manifest.new\"" < "$binary.matter-manifest"
    ssh_command "chmod 600 \"$target/.microhad-zigbee-native.matter-manifest.new\"; mv \"$target/.microhad-zigbee-native.matter-manifest.new\" \"$target/tuya-zigbee-native.matter-manifest\""
    ssh_command "dd of=\"$target/.thread-matter.new\"" < "$matter_service"
    ssh_command "chmod 700 \"$target/.thread-matter.new\"; mv \"$target/.thread-matter.new\" \"$target/thread-matter\""
    ssh_command "dd of=\"$target/.paired_start.sh.new\"" < "$project_dir/paired_start.sh"
    ssh_command "chmod 700 \"$target/.paired_start.sh.new\"; mv \"$target/.paired_start.sh.new\" \"$target/paired_start.sh\""
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
if [ "$activate_paired" = true ] || [ "$activate_paired_only" = true ]; then
    ssh_command "dd of=\"$target/.gateway_start.sh.new\"" < "$project_dir/ziggurat_start.sh"
    ssh_command "test -f /tuya/tuya_start.sh.lowjs-backup || cp /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup; chmod 755 \"$target/.gateway_start.sh.new\"; mv \"$target/.gateway_start.sh.new\" /tuya/tuya_start.sh"
fi
if [ "$restart" = true ]; then
    printf 'Restarting the supervised native daemon\n'
    ssh_command "killall -TERM microhad-zigbee-native >/dev/null 2>&1 || true
is_deployed_binary_running() {
    for executable in /proc/[0-9]*/exe; do
        [ -e \"\$executable\" ] || continue
        [ \"\$executable\" -ef \"$target/tuya-zigbee-native\" ] && return 0
    done
    return 1
}
retries=0
while [ \$retries -lt 13 ]; do
    if is_deployed_binary_running; then exit 0; fi
    retries=\$((retries + 1))
    sleep 1
done
echo 'ERROR: restart check failed: deployed microhad-zigbee-native binary was not running within 13 seconds' >&2
ps w >&2
exit 1"
    echo "Selected assets uploaded and service restarted."
elif [ "$activate" = true ]; then
    echo "Selected assets uploaded and activated for the next boot. Existing processes were not restarted."
elif [ "$activate_paired" = true ] || [ "$activate_paired_only" = true ]; then
    echo "Paired Matter-capable launcher activated for the next boot. Existing processes were not restarted."
else
    echo "Selected assets uploaded. Existing services were not changed."
fi
