#!/bin/sh
# Upload selected native gateway assets and an optional runtime config to a
# Dropbear-only gateway gateway. The server has no SCP subsystem, so files are
# transferred through SSH stdin and moved into place only after each transfer.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir="$repo_root/microhad"
. "$repo_root/scripts/workspace-env.sh"
workspace_load "$repo_root"
host=$GATEWAY_IP
user=$SSH_USER
target=/tuya/native-test
build_subdir=$(python3 "$repo_root/scripts/toolchain_config.py" TARGET_BUILD_SUBDIR)
binary="$project_dir/build/${build_subdir}mips-ziggurat-matter/microhad"
binary_explicit=false
password_file=$SSH_PASSWORD_FILE
config=""
deploy_binary=true
deploy_webui=true
deploy_supervisor=false
activate=false
activate_paired=false
activate_paired_only=false
restart=false
certificate=""
private_key=""
ca_bundle=""
matter_service=""
ziggurat=""
maintenance_helper=""
webui_dir="$project_dir/webui/dist"
maintenance_deploy=false
rollback_dir=""
service_selection=""
rollback_notice="No rollback artifacts were requested."
flash_reserve_kib=${DEPLOY_FLASH_RESERVE_KIB:-512}

usage() {
    echo "usage: $0 --password-file PATH --maintenance-deploy --binary PATH --ziggurat PATH --matter-service PATH --maintenance-helper PATH [--webui-dir DIR] [--rollback-dir DIR] [--host HOST] [--target /tuya/native]" >&2
    echo "       $0 --password-file PATH --services matter[,native,ziggurat,supervisor] --target /tuya/native [--matter-service PATH] [--binary PATH] [--ziggurat PATH] [--rollback-dir DIR]" >&2
    echo "       $0 --password-file PATH [--both --supervisor --binary PATH --target /tuya/native] [existing deployment options]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config) config=${2-}; shift 2 ;;
        --password-file) password_file=${2-}; SSH_PASSWORD=; shift 2 ;;
        --binary-only) deploy_binary=true; deploy_webui=false; shift ;;
        --webui-only) deploy_binary=false; deploy_webui=true; shift ;;
        --both) deploy_binary=true; deploy_webui=true; shift ;;
        --supervisor) deploy_supervisor=true; shift ;;
        --host) host=${2-}; shift 2 ;;
        --user) user=${2-}; shift 2 ;;
        --target) target=${2-}; shift 2 ;;
        --binary) binary=${2-}; binary_explicit=true; shift 2 ;;
        --matter-service) matter_service=${2-}; shift 2 ;;
        --ziggurat) ziggurat=${2-}; shift 2 ;;
        --maintenance-helper) maintenance_helper=${2-}; shift 2 ;;
        --webui-dir) webui_dir=${2-}; shift 2 ;;
        --maintenance-deploy) maintenance_deploy=true; shift ;;
        --services) service_selection=${2-}; [ -n "$service_selection" ] || usage; shift 2 ;;
        --matter-only) service_selection=matter; shift ;;
        --ziggurat-only) service_selection=ziggurat; shift ;;
        --rollback-dir) rollback_dir=${2-}; shift 2 ;;
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

if [ -n "$service_selection" ]; then
    maintenance_deploy=true
    deploy_binary=false
    deploy_webui=false
    case "$service_selection" in ,*|*,|*,,*) usage ;; esac
    selection_seen=,
    selection_ifs=$IFS; IFS=,
    for selected in $service_selection; do
        case "$selection_seen" in *,$selected,*) echo "Duplicate service: $selected" >&2; exit 2 ;; esac
        selection_seen="$selection_seen$selected,"
        case "$selected" in
            matter) [ -n "$matter_service" ] && [ -x "$matter_service" ] || usage ;;
            native) [ "$binary_explicit" = true ] && [ -x "$binary" ] || usage ;;
            ziggurat) [ -n "$ziggurat" ] && [ -x "$ziggurat" ] || usage ;;
            supervisor) ;;
            *) echo "Unknown service: $selected" >&2; exit 2 ;;
        esac
    done
    IFS=$selection_ifs
    [ "$target" = /tuya/native ] || usage
fi
[ -n "$host" ] || { echo 'Set GATEWAY_IP or supply --host HOST' >&2; exit 2; }
[ -z "$password_file" ] || [ -z "$SSH_PASSWORD" ] || { echo 'Configure SSH_PASSWORD_FILE or SSH_PASSWORD, not both' >&2; exit 2; }
if [ -n "$password_file" ]; then
    [ -r "$password_file" ] || usage
else
    [ -n "$SSH_PASSWORD" ] || usage
fi
[ "$maintenance_deploy" = false ] || [ -n "$service_selection" ] || {
    [ -n "$binary" ] && [ -n "$ziggurat" ] && [ -n "$matter_service" ] &&
        [ -n "$maintenance_helper" ] && [ "$target" = /tuya/native ] &&
        [ "$binary_explicit" = true ] || usage
}
[ "$maintenance_deploy" = false ] || {
    [ -z "$config" ] && [ -z "$certificate" ] &&
        [ -z "$private_key" ] && [ -z "$ca_bundle" ] && [ "$activate" = false ] &&
        [ "$activate_paired" = false ] && [ "$activate_paired_only" = false ] &&
        [ "$restart" = false ] || usage
}
[ "$maintenance_deploy" = true ] || { [ -z "$ziggurat" ] && [ -z "$maintenance_helper" ] || {
    echo "--ziggurat and --maintenance-helper require --maintenance-deploy" >&2; exit 2;
}; }
[ "$deploy_binary" = true ] || [ "$deploy_webui" = true ] || [ -n "$service_selection" ] || [ "$activate_paired_only" = true ] || usage
[ -z "$matter_service" ] || [ -n "$service_selection" ] || [ "$binary_explicit" = true ] || {
    echo "--matter-service requires --binary from a ZIGGURAT=1 MATTER=1 build" >&2; exit 2;
}
[ "$deploy_supervisor" = false ] || { [ "$deploy_binary" = true ] && [ "$binary_explicit" = true ] && [ "$target" = /tuya/native ] && [ "$activate_paired_only" = false ]; } || { echo "--supervisor requires --binary and binary deployment to /tuya/native" >&2; exit 2; }
[ -n "$service_selection" ] || { [ -z "$matter_service" ] && [ "$deploy_supervisor" = false ]; } || {
    manifest="$binary.matter-manifest"
    [ -f "$manifest" ] || { echo "Matter build manifest missing: $manifest" >&2; exit 2; }
    grep -qx 'format=microhad-build-v1' "$manifest" &&
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
[ "$deploy_webui" = false ] || { [ -f "$webui_dir/index.html.gz" ] && [ -f "$webui_dir/sw.js.gz" ]; } || { echo "Built gzip Web UI not found; run make webui first" >&2; exit 1; }
[ -z "$config" ] || { [ "$deploy_binary" = true ] && [ -f "$config" ]; } || usage
[ -z "$matter_service" ] || [ -n "$service_selection" ] || { { [ "$deploy_binary" = true ] || [ "$activate_paired_only" = true ]; } && [ -x "$matter_service" ]; } || usage
[ "$maintenance_deploy" = false ] || [ -n "$service_selection" ] || { [ -x "$ziggurat" ] && [ -x "$maintenance_helper" ]; } || { echo "Ziggurat or gateway-maintenance artifact missing or not executable" >&2; exit 1; }
[ "$maintenance_deploy" = false ] || [ -n "$service_selection" ] || { [ -d "$webui_dir" ] && [ -f "$webui_dir/index.html.gz" ] && [ -f "$webui_dir/sw.js.gz" ]; } || usage
[ -z "$certificate" ] || [ -f "$certificate" ] || usage
[ -z "$private_key" ] || [ -f "$private_key" ] || usage
[ -z "$ca_bundle" ] || [ -f "$ca_bundle" ] || usage
[ -z "$certificate" ] && [ -z "$private_key" ] || { [ -n "$certificate" ] && [ -n "$private_key" ] || usage; }
[ "$flash_reserve_kib" -ge 0 ] 2>/dev/null || {
    echo "DEPLOY_FLASH_RESERVE_KIB must be a non-negative integer" >&2; exit 2;
}
[ "$maintenance_deploy" = false ] || [ "$flash_reserve_kib" -ge 512 ] || {
    echo "--maintenance-deploy requires at least the 512 KiB flash reserve" >&2; exit 2;
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
[ "$activate_paired_only" = false ] || { [ "$target" = /tuya/native ] && [ "$activate_paired" = false ] && [ "$activate" = false ] && [ "$restart" = false ] && [ -z "$config" ] && [ -n "$matter_service" ] && [ "$binary_explicit" = true ] && [ -z "$certificate" ] && [ -z "$private_key" ] && [ -z "$ca_bundle" ]; } || { echo "--activate-paired-only requires --target /tuya/native, --binary, and --matter-service; it cannot be combined with uploads or restart" >&2; exit 2; }
[ "$restart" = false ] || [ "$target" = /tuya/native ] || { echo "--restart requires --target /tuya/native" >&2; exit 2; }
[ "$deploy_binary" = true ] || { [ -z "$config" ] && [ "$activate" = false ] && [ "$activate_paired" = false ] && [ "$restart" = false ]; } || { echo "--config, launcher activation, and --restart require binary deployment" >&2; exit 2; }

ssh_command() {
    remote_command=$1
    # The wrapper sources inline credentials itself; only a pathname is passed.
    if [ -n "$password_file" ]; then
        set -- --password-file "$password_file"
    else
        set --
    fi
    sh "$repo_root/scripts/workspace-ssh.sh" "$@" --user "$user" -- "$host" "$remote_command" || {
            status=$?
            printf 'ERROR: SSH command failed for %s@%s (exit %s): %s\n' \
                "$user" "$host" "$status" "$remote_command" >&2
            return "$status"
        }
}

start_remote_launcher() {
    # Detach set_time.sh too; the boot wrapper backgrounds it without closing
    # inherited SSH descriptors.
    ssh_command "( /tuya/tuya_start.sh </dev/null >/dev/null 2>&1 ) </dev/null >/dev/null 2>&1 &"
}

verification_get() {
    curl --noproxy '*' --connect-timeout 5 --max-time "${2:-10}" -fsS "http://$host$1"
}

verification_headers() {
    curl --noproxy '*' --connect-timeout 5 --max-time "${2:-10}" -fsS -D - -o /dev/null "http://$host$1"
}

wait_for_dashboard() {
    echo "Waiting for the HTTP dashboard to become ready"
    sleep 3
    dashboard_deadline=$(($(date +%s) + 30))
    while ! verification_get /api/status 2 >/dev/null 2>&1; do
        if [ "$(date +%s)" -ge "$dashboard_deadline" ]; then
            echo "HTTP dashboard did not become ready within 30 seconds" >&2
            return 1
        fi
        sleep 1
    done
}

verify_webui_release() {
    release_dir=$1
    [ -f "$release_dir/index.html.gz" ] && [ -f "$release_dir/sw.js.gz" ] || {
        echo "gzip Web UI entry assets are missing from $release_dir" >&2; return 1;
    }
    find "$release_dir" -type f | while IFS= read -r asset; do
        case "$asset" in *.gz) ;; *) echo "Uncompressed Web UI artifact: $asset" >&2; exit 1 ;; esac
    done
}

verify_webui_assets() {
    release_dir=$1
    verify_webui_release "$release_dir" || return 1
    find "$release_dir" -type f -name '*.gz' | while IFS= read -r asset; do
        relative=${asset#"$release_dir"/}
        asset_url=/${relative%.gz}
        headers=$(verification_headers "$asset_url") || {
            echo "Web UI asset is not served: $asset_url" >&2; exit 1;
        }
        printf '%s\n' "$headers" | tr -d '\r' | grep -qi '^Content-Encoding: gzip$' || {
            echo "Web UI asset is missing gzip content encoding: $asset_url" >&2; exit 1;
        }
    done
}

# Selective maintenance keeps the installed launcher and uploads only changed
# executables. Stop the paired owner before replacing its children, preserving
# every configuration/identity/network file and the flash reserve.
selected_service_paths() {
    case "$1" in
        matter) service_source=$matter_service; service_name=mthread-matter ;;
        native) service_source=$binary; service_name=microhad ;;
        ziggurat) service_source=$ziggurat; service_name=ziggurat ;;
        supervisor) service_source="$project_dir/paired_start.sh"; service_name=paired_start.sh ;;
    esac
}
selected_services_deploy() {
    selected_readelf=${TARGET_READELF:-$(python3 "$repo_root/scripts/toolchain_config.py" TARGET_READELF)}
    [ -x "$selected_readelf" ] || { echo 'MIPS readelf is missing' >&2; return 1; }
    selected_words=$(printf '%s' "$service_selection" | tr ',' ' ')
    changed_services=
    for selected in $selected_words; do
        selected_service_paths "$selected"
        if [ "$selected" = supervisor ]; then
            sh -n "$service_source" && sh -n "$project_dir/scripts/paired_control.sh" || return 1
        else
            elf_header=$("$selected_readelf" -h "$service_source") || return 1
            printf '%s\n' "$elf_header" | grep -q 'Class:.*ELF32' &&
                printf '%s\n' "$elf_header" | grep -q 'Data:.*big endian' &&
                printf '%s\n' "$elf_header" | grep -q 'Machine:.*MIPS' || {
                    echo "Artifact is not big-endian MIPS: $service_source" >&2; return 1;
                }
        fi
        if [ "$selected" = native ]; then
            manifest="$binary.matter-manifest"
            [ -f "$manifest" ] && grep -qx 'format=microhad-build-v1' "$manifest" &&
                grep -qx 'ziggurat=1' "$manifest" && grep -qx 'matter=1' "$manifest" &&
                [ "$(sed -n 's/^binary_sha256=//p' "$manifest")" = "$(sha256sum "$binary" | awk '{print $1}')" ] || {
                    echo 'Selected native binary requires its matching Matter manifest' >&2; return 1;
                }
        fi
        local_hash=$(md5sum "$service_source" | awk '{print $1}')
        remote_hash=$(ssh_command "if [ -f \"$target/$service_name\" ]; then md5sum \"$target/$service_name\" | awk '{print \$1}'; fi" </dev/null) || return 1
        manifest_changed=false
        if [ "$selected" = supervisor ]; then
            remote_control_hash=$(ssh_command "if [ -f \"$target/paired_control.sh\" ]; then md5sum \"$target/paired_control.sh\" | awk '{print \$1}'; fi" </dev/null) || return 1
            [ "$remote_control_hash" = "$(md5sum "$project_dir/scripts/paired_control.sh" | awk '{print $1}')" ] || manifest_changed=true
        fi
        if [ "$selected" = native ]; then
            remote_manifest_hash=$(ssh_command "if [ -f \"$target/microhad.matter-manifest\" ]; then md5sum \"$target/microhad.matter-manifest\" | awk '{print \$1}'; fi" </dev/null) || return 1
            [ "$remote_manifest_hash" = "$(md5sum "$manifest" | awk '{print $1}')" ] || manifest_changed=true
        fi
        if [ "$local_hash" = "$remote_hash" ] && [ "$manifest_changed" = false ]; then
            echo "Unchanged service: $selected (skipping upload)"
        else
            changed_services="$changed_services $selected"
        fi
    done
    [ -n "$changed_services" ] || { echo 'Selected services are already current; no restart needed'; return 0; }
    ssh_command "test -x /tuya/tuya_start.sh && test -x \"$target/paired_control.sh\" && test -x \"$target/microhad\" && test -x \"$target/ziggurat\" && test -x \"$target/mthread-matter\" && grep -q '^matter=1$' \"$target/microhad.matter-manifest\" && test ! -e /tmp/ziggurat-supervisor/quiesce && test ! -e /tmp/ziggurat-supervisor/stop" || {
        echo 'Selective deployment requires a running paired Matter installation' >&2; return 1;
    }
    # Refuse an existing deleted image before changing any running service.
    ssh_command "for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> $target/microhad (deleted)\"|*\" -> $target/ziggurat (deleted)\"|*\" -> $target/mthread-matter (deleted)\") echo 'Running deleted executable; refusing deployment' >&2; exit 1 ;; esac; done" || return 1
    if [ -n "$rollback_dir" ]; then
        (umask 077 && mkdir -p "$rollback_dir") || return 1
        chmod 700 "$rollback_dir"
        [ -z "$(find "$rollback_dir" -mindepth 1 -print -quit)" ] || { echo 'Rollback directory must be empty' >&2; return 1; }
        for selected in $changed_services; do
            selected_service_paths "$selected"
            ssh_command "cat \"$target/$service_name\"" > "$rollback_dir/$service_name" || return 1
            saved_hash=$(ssh_command "md5sum \"$target/$service_name\" | awk '{print \$1}'" </dev/null) || return 1
            [ "$saved_hash" = "$(md5sum "$rollback_dir/$service_name" | awk '{print $1}')" ] || return 1
            if [ "$selected" = supervisor ]; then
                ssh_command "cat \"$target/paired_control.sh\"" > "$rollback_dir/paired_control.sh" || return 1
            fi
            if [ "$selected" = native ]; then
                ssh_command "cat \"$target/microhad.matter-manifest\"" > "$rollback_dir/microhad.matter-manifest" || return 1
            fi
        done
        rollback_notice="Selected previous application files are preserved at $rollback_dir."
    else
        echo 'Rollback artifact generation disabled'
    fi
    # Check raw one-file staging headroom before taking the stack offline.
    available=$(ssh_command "df -k /tuya | awk 'END {print \$4}'" </dev/null) || return 1
    case "$available" in ''|*[!0-9]*) echo 'Unable to read flash headroom' >&2; return 1 ;; esac
    for selected in $changed_services; do
        selected_service_paths "$selected"
        staging_bytes=$(wc -c < "$service_source")
        if [ "$selected" = supervisor ]; then
            staging_bytes=$((staging_bytes + $(wc -c < "$project_dir/scripts/paired_control.sh")))
        fi
        needed=$(((staging_bytes + 1023) / 1024 + flash_reserve_kib))
        [ "$available" -ge "$needed" ] || { echo "Insufficient staging space for $selected; installation unchanged" >&2; return 1; }
    done
    supervisor_pid=$(ssh_command 'cat /tmp/ziggurat-supervisor/supervisor.pid' </dev/null) || return 1
    case "$supervisor_pid" in ''|*[!0-9]*) echo 'Invalid supervisor PID' >&2; return 1 ;; esac
    [ "$supervisor_pid" -gt 1 ] || return 1
    echo "Updating services:$changed_services; quiescing the paired stack"
    ssh_command "kill -0 $supervisor_pid && : > /tmp/ziggurat-supervisor/quiesce; n=0; while [ ! -f /tmp/ziggurat-supervisor/quiesced ] && [ \$n -lt 20 ]; do sleep 1; n=\$((n+1)); done; test -f /tmp/ziggurat-supervisor/quiesced" || return 1
    ssh_command "kill -TERM $supervisor_pid; n=0; while [ -d /proc/$supervisor_pid ] && [ \$n -lt 20 ]; do sleep 1; n=\$((n+1)); done; test ! -d /proc/$supervisor_pid" || return 1
    ssh_command "for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> $target/microhad\"|*\" -> $target/ziggurat\"|*\" -> $target/mthread-matter\") echo 'Paired process remains active' >&2; exit 1 ;; esac; done" || return 1
    selected_upload() {
        upload_source=$1; upload_destination=$2; upload_mode=$3
        upload_hash=$(md5sum "$upload_source" | awk '{print $1}')
        # Measure JFFS2 after transfer and verify before replacing the old file.
        ssh_command "dd of=\"$upload_destination.new\"" < "$upload_source" || {
            ssh_command "rm -f \"$upload_destination.new\"" </dev/null || true; return 1;
        }
        ssh_command "available=\$(df -k /tuya | awk 'END {print \$4}'); case \"\$available\" in ''|*[!0-9]*) rm -f \"$upload_destination.new\"; exit 1 ;; esac; if [ \"\$available\" -lt $flash_reserve_kib ] || [ \"\$(md5sum \"$upload_destination.new\" | awk '{print \$1}')\" != '$upload_hash' ]; then rm -f \"$upload_destination.new\"; exit 1; fi; chmod $upload_mode \"$upload_destination.new\" && mv \"$upload_destination.new\" \"$upload_destination\"" </dev/null || return 1
    }
    for selected in $changed_services; do
        selected_service_paths "$selected"
        selected_upload "$service_source" "$target/$service_name" 700 || {
            echo "Deployment incomplete; services remain stopped. $rollback_notice" >&2; return 1;
        }
        if [ "$selected" = supervisor ]; then
            selected_upload "$project_dir/scripts/paired_control.sh" "$target/paired_control.sh" 700 || return 1
        fi
        if [ "$selected" = native ]; then
            selected_upload "$binary.matter-manifest" "$target/microhad.matter-manifest" 600 || return 1
        fi
    done
    start_remote_launcher || return 1
    wait_for_dashboard || return 1
    ssh_command "n=0; while [ \$n -lt 30 ]; do if [ -S /tmp/ziggurat/thread.sock ] && [ -S /tmp/microhad-matter.sock ] && [ -f /tmp/ziggurat-supervisor/matter.pid ] && kill -0 \$(cat /tmp/ziggurat-supervisor/matter.pid) 2>/dev/null; then exit 0; fi; sleep 1; n=\$((n+1)); done; exit 1" || return 1
    selected_status=$(verification_get /api/matter/status) || return 1
    printf '%s' "$selected_status" | python3 -c 'import json,sys; s=json.load(sys.stdin); sys.exit(0 if s.get("available") and s.get("controller_initialized") and s.get("fabric_ready") and s.get("state")=="running" else 1)' || {
        echo "Matter service did not become ready. $rollback_notice" >&2; return 1;
    }
    echo "Selected service deployment complete:$changed_services"
}

maintenance_deploy_run() {
    [ "$maintenance_deploy" = true ] || return 0
    [ "$deploy_binary" = true ] && [ "$deploy_webui" = true ] || {
        echo "--maintenance-deploy always installs the complete artifact bundle" >&2; return 2;
    }
    [ "$target" = /tuya/native ] || return 2
    command -v strings >/dev/null 2>&1 || { echo "strings is required to verify the Ziggurat interface" >&2; return 1; }
    ziggurat_strings=$(strings "$ziggurat") || return 1
    printf '%s\n' "$ziggurat_strings" | grep -Fq -- 'Thread endpoint requires --spinel-iid 1' &&
        printf '%s\n' "$ziggurat_strings" | grep -Fq -- 'cobs-unix requires unix:/absolute/path' || {
            echo "Ziggurat artifact lacks the multipan/COBS Unix and Thread socket interfaces; rebuild with ZIGGURAT_MULTIPAN=1" >&2
            return 1
        }
    [ -f "$binary.matter-manifest" ] || { echo "Matter build manifest missing: $binary.matter-manifest" >&2; return 1; }
    grep -qx 'format=microhad-build-v1' "$binary.matter-manifest" &&
        grep -qx 'ziggurat=1' "$binary.matter-manifest" &&
        grep -qx 'matter=1' "$binary.matter-manifest" || {
            echo "Matter build manifest does not identify an enabled Ziggurat Matter binary" >&2; return 1;
        }
    expected_hash=$(sed -n 's/^binary_sha256=//p' "$binary.matter-manifest")
    [ -n "$expected_hash" ] && [ "$expected_hash" = "$(sha256sum "$binary" | awk '{print $1}')" ] || {
        echo "Matter build manifest does not match the selected native binary" >&2; return 1;
    }
    command -v tar >/dev/null 2>&1 || { echo "tar is required for rollback-safe Web UI backup" >&2; return 1; }
    command -v curl >/dev/null 2>&1 || { echo "curl is required to verify the deployed Web UI and API" >&2; return 1; }
    target_readelf=${TARGET_READELF:-$(python3 "$repo_root/scripts/toolchain_config.py" TARGET_READELF)}
    [ -x "$target_readelf" ] || { echo "MIPS readelf is missing: $target_readelf" >&2; return 1; }
    for artifact in "$binary" "$ziggurat" "$matter_service" "$maintenance_helper"; do
        elf_header=$("$target_readelf" -h "$artifact") || return 1
        printf '%s\n' "$elf_header" | grep -q 'Class:.*ELF32' &&
            printf '%s\n' "$elf_header" | grep -q 'Data:.*big endian' &&
            printf '%s\n' "$elf_header" | grep -q 'Machine:.*MIPS' || {
                echo "Artifact is not a 32-bit big-endian MIPS executable: $artifact" >&2; return 1;
            }
    done
    find "$webui_dir" -type f | while IFS= read -r asset; do
        relative=${asset#"$webui_dir"/}
        case "$relative" in *[!A-Za-z0-9_./-]*|*..*) echo "Unsupported Web UI path: $relative" >&2; exit 1 ;; esac
    done || { echo "Web UI contains an unsupported asset path" >&2; return 1; }

    ssh_command "test -f /tuya/tuya_start.sh" || { echo "Installed launcher is missing; cannot safely activate or restart" >&2; return 1; }
    if [ -n "$rollback_dir" ]; then
        rollback_notice="Previous application files are preserved at $rollback_dir for manual recovery."
        (umask 077 && mkdir -p "$rollback_dir") || return 1
        chmod 700 "$rollback_dir"
        [ -z "$(find "$rollback_dir" -mindepth 1 -print -quit)" ] || {
            echo "Rollback directory must be empty: $rollback_dir" >&2; return 2;
        }
        echo "Rollback files will be kept at $rollback_dir"

        # Backup only application artifacts. Runtime configuration, Matter identity,
        # Zigbee state, TLS material and network backups are intentionally excluded.
        for name in microhad microhad.matter-manifest thread-matter mthread-matter ziggurat gateway-maintenance paired_start.sh paired_control.sh; do
            remote="$target/$name"
            if ssh_command "test -f \"$remote\"" >/dev/null 2>&1; then
                ssh_command "cat \"$remote\"" > "$rollback_dir/$name" || return 1
                remote_hash=$(ssh_command "md5sum \"$remote\" | awk '{print \$1}'") || return 1
                local_hash=$(md5sum "$rollback_dir/$name" | awk '{print $1}')
                [ "$remote_hash" = "$local_hash" ] || { echo "Backup verification failed for $remote" >&2; return 1; }
            else
                : > "$rollback_dir/$name.absent"
            fi
        done
        if ssh_command "test -f /tuya/tuya_start.sh" >/dev/null 2>&1; then
            ssh_command "cat /tuya/tuya_start.sh" > "$rollback_dir/gateway_start.sh" || return 1
            remote_hash=$(ssh_command "md5sum /tuya/tuya_start.sh | awk '{print \$1}'") || return 1
            local_hash=$(md5sum "$rollback_dir/gateway_start.sh" | awk '{print $1}')
            [ "$remote_hash" = "$local_hash" ] || { echo "Launcher backup verification failed" >&2; return 1; }
        else
            : > "$rollback_dir/gateway_start.sh.absent"
        fi
        if ssh_command "test -d \"$target/webui\"" >/dev/null 2>&1; then
            ssh_command "tar -cf - -C \"$target\" webui" > "$rollback_dir/webui.tar" || return 1
            tar -tf "$rollback_dir/webui.tar" >/dev/null || { echo "Web UI backup archive is invalid" >&2; return 1; }
        else
            : > "$rollback_dir/webui.absent"
        fi

    else
        echo "Rollback artifact generation disabled (use --rollback-dir to opt in)"
    fi

    supervisor_pid=$(ssh_command "cat /tmp/ziggurat-supervisor/supervisor.pid 2>/dev/null || true") || return 1
    case "$supervisor_pid" in '' ) echo "No paired supervisor PID; checking for orphaned application processes" ;; *[!0-9]*) echo "Paired supervisor PID is invalid" >&2; return 1 ;; esac
    if [ -n "$supervisor_pid" ]; then
        ssh_command "test -d /proc/$supervisor_pid" || { echo "Recorded paired supervisor is not running" >&2; return 1; }
    fi
    ssh_command "test -x \"$target/gateway-maintenance\"" || {
        echo "Installed maintenance helper is not available" >&2; return 1;
    }
    if [ -z "$supervisor_pid" ]; then
        orphaned_images=$(ssh_command "for image in \"$target/microhad\" \"$target/thread-matter\" \"$target/mthread-matter\" \"$target/ziggurat\"; do for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> \$image\"|*\" -> \$image (deleted)\") echo \"\$x\" ;; esac; done; done; :") || return 1
        [ -z "$orphaned_images" ] || { echo "Application executable is still active without its paired supervisor: $orphaned_images" >&2; return 1; }
    fi
    deleted_images=$(ssh_command "for image in \"$target/microhad\" \"$target/thread-matter\" \"$target/mthread-matter\" \"$target/ziggurat\" \"$target/gateway-maintenance\"; do for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> \$image (deleted)\") echo \"\$x\" ;; esac; done; done; :") || return 1
    [ -z "$deleted_images" ] || { echo "Found a running deleted executable; refusing maintenance deployment" >&2; return 1; }

    if [ -n "$supervisor_pid" ]; then
        echo "Quiescing the paired stack and stopping its supervisor"
        ssh_command "test -d /tmp/ziggurat-supervisor && : > /tmp/ziggurat-supervisor/quiesce"
        ssh_command "n=0; while [ ! -f /tmp/ziggurat-supervisor/quiesced ] && [ \$n -lt 20 ]; do sleep 1; n=\$((n+1)); done; test -f /tmp/ziggurat-supervisor/quiesced" || {
            echo "Supervisor did not quiesce; deployment is stopping without replacing files" >&2
            ssh_command "kill -TERM $supervisor_pid 2>/dev/null || true; n=0; while [ -d /proc/$supervisor_pid ] && [ \$n -lt 15 ]; do sleep 1; n=\$((n+1)); done; test ! -d /proc/$supervisor_pid" || true
            return 1
        }
        ssh_command "kill -TERM $supervisor_pid; n=0; while [ -d /proc/$supervisor_pid ] && [ \$n -lt 15 ]; do sleep 1; n=\$((n+1)); done; test ! -d /proc/$supervisor_pid" || {
            echo "Supervisor did not stop cleanly; no artifacts were replaced" >&2; return 1;
        }
    else
        echo "Paired services are already stopped"
    fi
    ssh_command "for image in \"$target/microhad\" \"$target/thread-matter\" \"$target/mthread-matter\" \"$target/ziggurat\" \"$target/gateway-maintenance\"; do for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> \$image\"|*\" -> \$image (deleted)\") echo \"Executable still active: \$x\" >&2; exit 1 ;; esac; done; done; :" || {
        echo "A deployment executable is still active after supervisor shutdown; refusing replacement" >&2
        return 1
    }
    old_service_size=$(ssh_command "for service in \"$target/thread-matter\" \"$target/mthread-matter\"; do if [ -f \"\$service\" ]; then wc -c < \"\$service\"; fi; done | awk '{total += \$1} END {print total + 0}'") || return 1
    old_ziggurat_size=$(ssh_command "if [ -f \"$target/ziggurat\" ]; then wc -c < \"$target/ziggurat\" | awk '{print \$1}'; else echo 0; fi") || return 1
    if [ -f "$rollback_dir/webui.tar" ]; then
        old_webui_size=$(tar -tvf "$rollback_dir/webui.tar" | awk '$3 ~ /^[0-9]+$/ { total += $3 } END { print total + 0 }')
    else
        old_webui_size=0
    fi
    webui_bytes=$(find "$webui_dir" -type f -exec wc -c {} + | awk '$2 != "total" { total += $1 } END { print total + 0 }')
    binary_bytes=$(wc -c < "$binary"); ziggurat_bytes=$(wc -c < "$ziggurat")
    service_bytes=$(wc -c < "$matter_service"); helper_bytes=$(wc -c < "$maintenance_helper")
    manifest_bytes=$(wc -c < "$binary.matter-manifest")
    support_bytes=$(($(wc -c < "$project_dir/paired_start.sh") + $(wc -c < "$project_dir/scripts/paired_control.sh")))
    launcher_bytes=$(wc -c < "$project_dir/ziggurat_start.sh")
    launcher_backup_bytes=0
    [ -z "$rollback_dir" ] || launcher_backup_bytes=$(ssh_command "if [ -e /tuya/tuya_start.sh.lowjs-backup ]; then echo 0; elif [ -f /tuya/tuya_start.sh ]; then wc -c < /tuya/tuya_start.sh | awk '{print \$1}'; else echo 0; fi") || return 1
    webui_largest=$(find "$webui_dir" -type f -exec wc -c {} + |
        awk '$2 != "total" && $1 > largest { largest = $1 } END { print largest + 0 }')
    largest_artifact=$binary_bytes
    for artifact_size in "$ziggurat_bytes" "$service_bytes" "$helper_bytes" "$manifest_bytes" "$support_bytes" "$launcher_bytes" "$launcher_backup_bytes" "$webui_largest"; do
        [ "$artifact_size" -le "$largest_artifact" ] || largest_artifact=$artifact_size
    done
    # JFFS2 compresses writes, so a raw whole-release sum is too pessimistic.
    # Check one-file headroom here, then measure actual JFFS2 space after each
    # atomic temporary write. Raw artifact sizes overstate flash usage.
    required_kib=$(((largest_artifact + 1023) / 1024 + flash_reserve_kib))
    available_kib=$(ssh_command "df -k /tuya | awk 'END {print \$4}'") || return 1
    case "$available_kib:$required_kib:$old_service_size:$old_webui_size:$launcher_backup_bytes" in *[!0-9:]*) echo "Unable to calculate Matter bundle flash requirements" >&2; return 1 ;; esac
    case "$old_ziggurat_size" in ''|*[!0-9]*) echo "Unable to read old Ziggurat size" >&2; return 1 ;; esac
    predicted_after_reclaim=$((available_kib + (old_service_size + old_ziggurat_size + old_webui_size + 1023) / 1024))
    if [ "$predicted_after_reclaim" -lt "$required_kib" ]; then
        echo "Insufficient flash even after reclaiming the stopped Matter service, Ziggurat and Web UI: need ${required_kib} KiB including reserve, have ${predicted_after_reclaim} KiB" >&2
        ssh_command "rm -f /tmp/ziggurat/native.sock /tmp/ziggurat/thread.sock /tmp/microhad-matter.sock" || true
        return 1
    fi

    upload_atomic() {
        source_file=$1; destination=$2; mode=$3
        temporary="$destination.new"
        remote_directory=${destination%/*}
        available_before=$(ssh_command "df -k /tuya | awk 'END {print \$4}'" </dev/null) || return 1
        case "$available_before" in ''|*[!0-9]*) echo "Unable to read flash before staging $destination" >&2; return 1 ;; esac
        if [ "$available_before" -le "$flash_reserve_kib" ]; then
            echo "Not enough free flash to begin staging $destination while retaining ${flash_reserve_kib} KiB reserve: have ${available_before} KiB" >&2
            return 1
        fi
        ssh_command "mkdir -p \"$remote_directory\"; rm -f \"$temporary\"" </dev/null
        ssh_command "dd of=\"$temporary\"" < "$source_file" || return 1
        available_after=$(ssh_command "df -k /tuya | awk 'END {print \$4}'" </dev/null) || {
            ssh_command "rm -f \"$temporary\"" </dev/null || true; return 1;
        }
        case "$available_after" in ''|*[!0-9]*) ssh_command "rm -f \"$temporary\"" </dev/null || true; echo "Unable to verify flash after staging $destination" >&2; return 1 ;; esac
        if [ "$available_after" -lt "$flash_reserve_kib" ]; then
            ssh_command "rm -f \"$temporary\"" </dev/null || true
            echo "Staging $destination would consume the flash reserve; removed its temporary copy" >&2
            return 1
        fi
        ssh_command "chmod $mode \"$temporary\" && mv \"$temporary\" \"$destination\"" </dev/null || return 1
        local_hash=$(md5sum "$source_file" | awk '{print $1}')
        remote_hash=$(ssh_command "md5sum \"$destination\" | awk '{print \$1}'" </dev/null) || return 1
        [ "$local_hash" = "$remote_hash" ] || { echo "Hash verification failed for $destination" >&2; return 1; }
    }
    ssh_command "rm -f \"$target/thread-matter\" \"$target/mthread-matter\" \"$target/ziggurat\"; rm -rf \"$target/webui\""
    available_kib=$(ssh_command "df -k /tuya | awk 'END {print \$4}'")
    case "$available_kib" in ''|*[!0-9]*) echo "Unable to reread available /tuya flash space" >&2; return 1 ;; esac
    if [ "$available_kib" -lt "$required_kib" ]; then
        echo "Flash preflight failed after safe reclamation: need ${required_kib} KiB including reserve, have ${available_kib} KiB" >&2
        return 1
    fi

    # Only remove stale IPC endpoints after verifying all old socket owners are
    # gone. Ziggurat and the Matter service cannot bind to an existing pathname.
    ssh_command "rm -f /tmp/ziggurat/native.sock /tmp/ziggurat/thread.sock /tmp/microhad-matter.sock"

    if ! upload_atomic "$binary" "$target/microhad" 700 ||
        ! upload_atomic "$binary.matter-manifest" "$target/microhad.matter-manifest" 600 ||
        ! upload_atomic "$ziggurat" "$target/ziggurat" 700 ||
        ! upload_atomic "$maintenance_helper" "$target/gateway-maintenance" 700 ||
        ! upload_atomic "$matter_service" "$target/mthread-matter" 700 ||
        ! upload_atomic "$project_dir/paired_start.sh" "$target/paired_start.sh" 700 ||
        ! upload_atomic "$project_dir/scripts/paired_control.sh" "$target/paired_control.sh" 700; then
        echo "Deployment incomplete; services remain stopped. $rollback_notice" >&2
        return 1
    fi
    find "$webui_dir" -type f ! -name index.html.gz | while IFS= read -r asset; do
        relative=${asset#"$webui_dir"/}
        case "$relative" in *[!A-Za-z0-9_./-]*|*..*) echo "Unsupported Web UI path: $relative" >&2; exit 1 ;; esac
        upload_atomic "$asset" "$target/webui/$relative" 644 || exit 1
    done || { echo "Deployment incomplete; services remain stopped. $rollback_notice" >&2; return 1; }
    upload_atomic "$webui_dir/index.html.gz" "$target/webui/index.html.gz" 644 || { echo "Deployment incomplete; services remain stopped. $rollback_notice" >&2; return 1; }
    if [ "$launcher_backup_bytes" -gt 0 ]; then
        upload_atomic "$rollback_dir/gateway_start.sh" /tuya/tuya_start.sh.lowjs-backup 755 || { echo "Deployment incomplete; services remain stopped. $rollback_notice" >&2; return 1; }
    fi
    upload_atomic "$project_dir/ziggurat_start.sh" /tuya/tuya_start.sh 755 || { echo "Deployment incomplete; services remain stopped. $rollback_notice" >&2; return 1; }
    start_remote_launcher || { echo "Deployment staged, but launcher start failed. Services remain stopped; $rollback_notice" >&2; return 1; }

    ssh_command "n=0; while [ \$n -lt 30 ]; do if [ -f /tmp/ziggurat-supervisor/supervisor.pid ] && [ -S /tmp/ziggurat/thread.sock ] && [ -S /tmp/microhad-matter.sock ] && kill -0 \$(cat /tmp/ziggurat-supervisor/supervisor.pid) 2>/dev/null; then native=0;zig=0;matter=0; for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> $target/microhad\") native=1 ;; *\" -> $target/ziggurat\") zig=1 ;; *\" -> $target/mthread-matter\") matter=1 ;; esac; done; [ \$native -eq 1 ] && [ \$zig -eq 1 ] && [ \$matter -eq 1 ] && exit 0; fi; sleep 1; n=\$((n+1)); done; exit 1" || {
        echo "New services did not become ready. No automatic rollback was attempted. $rollback_notice" >&2
        return 1
    }
    wait_for_dashboard &&
        verification_get / >/dev/null &&
        verification_get /api/status >/dev/null &&
        verification_get /api/logs | grep -q '"lines"' || {
            echo "Web UI or status API did not respond" >&2
            echo "Dashboard verification failed. No automatic rollback was attempted. $rollback_notice" >&2
            return 1
        }
    verify_webui_assets "$webui_dir" || {
        echo "Web UI bundle verification failed. $rollback_notice" >&2
        return 1
    }
    runtime_report=$(ssh_command "df -k /tuya | awk 'END {print \"microhad_available_kib=\" \$4}'; awk '/^(MemFree|MemAvailable):/ {print}' /proc/meminfo; for p in /proc/[0-9]*/exe; do x=\$(ls -l \"\$p\" 2>/dev/null) || continue; case \"\$x\" in *\" -> $target/microhad\"|*\" -> $target/ziggurat\"|*\" -> $target/mthread-matter\") pid=\${p#/proc/}; pid=\${pid%/exe}; awk -v pid=\$pid '/^VmRSS:/ {print \"pid=\" pid, \$0}' /proc/\$pid/status 2>/dev/null ;; esac; done") || return 1
    printf 'Full Matter deployment is running. Runtime report:\n%s\nRollback backup: %s\n' "$runtime_report" "${rollback_dir:-disabled}"
    return 0
}

if [ -n "$service_selection" ]; then
    selected_services_deploy
    exit $?
fi
maintenance_deploy_run || exit $?
if [ "$maintenance_deploy" = true ]; then exit 0; fi

matter_flash_preflight() {
    [ "$activate_paired_only" = false ] || return 0
    [ -z "$matter_service" ] && [ "$deploy_supervisor" = false ] && return 0


    old_sizes=$(ssh_command "for file in \"$target/microhad\" \"$target/config.json\" \"$target/microhad.matter-manifest\" \"$target/mthread-matter\" \"$target/paired_start.sh\" \"$target/paired_control.sh\"; do if [ -f \"\$file\" ]; then wc -c < \"\$file\"; else echo 0; fi; done") || return $?
    set -- $old_sizes
    old_native=${1:-0}
    old_config=${2:-0}
    old_manifest=${3:-0}
    old_service=${4:-0}
    old_supervisor=${5:-0}
    old_control=${6:-0}
    for old_size in "$old_native" "$old_config" "$old_manifest" "$old_service" "$old_supervisor" "$old_control"; do
        case "$old_size" in ''|*[!0-9]*) echo "Unable to read existing Matter deployment file sizes" >&2; return 1 ;; esac
    done

    running_images=$(ssh_command "[ -d /proc/1 ] || exit 1; for proc in /proc/[0-9]*/exe; do link=\$(ls -l \"\$proc\" 2>/dev/null) || continue; case \"\$link\" in *\" -> $target/thread-matter (deleted)\") echo 'Legacy Matter executable is running from a deleted file; use maintenance deployment' >&2; exit 1 ;; esac; done; for image in \"$target/microhad\" \"$target/mthread-matter\"; do active=0; deleted=0; seen=0; for proc in /proc/[0-9]*/exe; do link=\$(ls -l \"\$proc\" 2>/dev/null) || continue; seen=1; case \"\$link\" in *\" -> \$image (deleted)\") deleted=1 ;; *\" -> \$image\") active=1 ;; esac; done; [ \"\$seen\" -eq 1 ] || exit 1; echo \"\$active:\$deleted\"; done") || {
        echo "Unable to inspect running executable references under /proc" >&2
        return 1
    }
    set -- $running_images
    native_image_state=${1:-}
    service_image_state=${2:-}
    case "$native_image_state" in 0:0|1:0) ;; *)
        echo "Cannot safely estimate flash: native executable is already running from a deleted file" >&2
        return 1
    esac
    case "$service_image_state" in 0:0|1:0) ;; *)
        echo "Cannot safely estimate flash: Matter service is already running from a deleted file" >&2
        return 1
    esac
    native_running=${native_image_state%%:*}
    service_running=${service_image_state%%:*}

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
    [ "$native_running" = 0 ] || replaced_native_and_config=0
    stage_flash_group "$((binary_size + config_size))" "$replaced_native_and_config"
    manifest_size=$(wc -c < "$binary.matter-manifest")
    stage_flash_group "$manifest_size" "$old_manifest"
    if [ -n "$matter_service" ]; then
        service_size=$(wc -c < "$matter_service")
        replaced_service=$old_service
        [ "$service_running" = 0 ] || replaced_service=0
        stage_flash_group "$service_size" "$replaced_service"
    fi
    supervisor_size=$(wc -c < "$project_dir/paired_start.sh")
    stage_flash_group "$supervisor_size" "$old_supervisor"
    control_size=$(wc -c < "$project_dir/scripts/paired_control.sh")
    stage_flash_group "$control_size" "$old_control"

    if [ "$deploy_webui" = true ]; then
        set -- $(find "$webui_dir" -type f -exec wc -c {} + |
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

    old_launcher_sizes=$(ssh_command "for file in /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup; do if [ -f \"\$file\" ]; then wc -c < \"\$file\"; else echo 0; fi; done") || return $?
    set -- $old_launcher_sizes
    old_launcher=${1:-0}
    old_backup=${2:-0}
    case "$old_launcher:$old_backup" in *[!0-9:]*) echo "Unable to read installed launcher sizes" >&2; return 1 ;; esac
    new_launcher=$(wc -c < "$project_dir/ziggurat_start.sh")
    backup_bytes=0
    [ -z "$rollback_dir" ] || [ "$old_backup" -ne 0 ] || backup_bytes=$old_launcher

    new_control_size=$(wc -c < "$project_dir/scripts/paired_control.sh")
    required_kib=$(((new_launcher + new_control_size + backup_bytes + 1023) / 1024 + flash_reserve_kib))
    available_kib=$(ssh_command "df -k /tuya | awk 'END {print \$4}'") || return $?
    case "$available_kib" in ''|*[!0-9]*) echo "Unable to read available /tuya flash space" >&2; return 1 ;; esac
    if [ "$available_kib" -lt "$required_kib" ]; then
        printf 'Insufficient flash for paired launcher and control helper: need %s KiB, have %s KiB\n' "$required_kib" "$available_kib" >&2
        return 1
    fi
    ssh_command "dd of=\"$target/.paired_control.sh.new\"" < "$project_dir/scripts/paired_control.sh"
    ssh_command "chmod 700 \"$target/.paired_control.sh.new\"; mv \"$target/.paired_control.sh.new\" \"$target/paired_control.sh\""

    ssh_command "test -x \"$target/microhad\" && test -x \"$target/mthread-matter\" && test -x \"$target/paired_start.sh\" && test -x \"$target/paired_control.sh\" && grep -q '^format=microhad-build-v1\$' \"$target/microhad.matter-manifest\" && grep -q '^ziggurat=1\$' \"$target/microhad.matter-manifest\" && grep -q '^matter=1\$' \"$target/microhad.matter-manifest\"" || {
        echo "Staged Matter package is incomplete or not executable" >&2
        return 1
    }
    expected_native_md5=$(md5sum "$binary" | awk '{print $1}')
    expected_manifest_md5=$(md5sum "$binary.matter-manifest" | awk '{print $1}')
    expected_service_md5=$(md5sum "$matter_service" | awk '{print $1}')
    expected_supervisor_md5=$(md5sum "$project_dir/paired_start.sh" | awk '{print $1}')
    expected_control_md5=$(md5sum "$project_dir/scripts/paired_control.sh" | awk '{print $1}')
    actual_matter_md5s=$(ssh_command "md5sum \"$target/microhad\" \"$target/microhad.matter-manifest\" \"$target/mthread-matter\" \"$target/paired_start.sh\" \"$target/paired_control.sh\" | awk '{print \$1}'") || return $?
    set -- $actual_matter_md5s
    [ "${1:-}" = "$expected_native_md5" ] &&
        [ "${2:-}" = "$expected_manifest_md5" ] &&
        [ "${3:-}" = "$expected_service_md5" ] &&
        [ "${4:-}" = "$expected_supervisor_md5" ] &&
        [ "${5:-}" = "$expected_control_md5" ] || {
            echo "Staged Matter package hashes do not match the selected local artifacts" >&2
            return 1
        }
    printf 'Paired launcher preflight passed: need at least %s KiB, have %s KiB available\n' "$required_kib" "$available_kib"
}

paired_launcher_preflight

prune_web_bundles() {
    assets_dir="$target/webui/assets"
    if [ "${1:-}" = final ]; then
        ssh_command "rm -f \"$target/webui/index.html\" \"$target/webui/index.html.br\" \"$target/webui/sw.js\" \"$target/webui/sw.js.br\" \"$target/webui/licenses.html\" \"$target/webui/licenses.html.br\" \"$target/webui/microhad-icon.svg\" \"$target/webui/microhad-icon.svg.br\" \"$assets_dir\"/index-*.js \"$assets_dir\"/index-*.css \"$assets_dir\"/index-*.js.br \"$assets_dir\"/index-*.css.br \"$assets_dir\"/index-*.js.gz.new \"$assets_dir\"/index-*.css.gz.new"
    else
        ssh_command "rm -f \"$assets_dir\"/index-*.js.gz.new \"$assets_dir\"/index-*.css.gz.new"
    fi
}

if [ "$deploy_binary" = true ]; then
    printf 'Uploading native binary to %s:%s\n' "$host" "$target"
    ssh_command "mkdir -p \"$target\"; rm -f \"$target/.microhad.new\" \"$target/.config.json.new\""
    ssh_command "dd of=\"$target/.microhad.new\"" < "$binary"
    if [ -z "$config" ]; then
        ssh_command "chmod 700 \"$target/.microhad.new\"; mv \"$target/.microhad.new\" \"$target/microhad\"; ls -l \"$target/microhad\""
    else
        ssh_command "dd of=\"$target/.config.json.new\"" < "$config"
        ssh_command "chmod 700 \"$target/.microhad.new\"; chmod 600 \"$target/.config.json.new\"; mv \"$target/.microhad.new\" \"$target/microhad\"; mv \"$target/.config.json.new\" \"$target/config.json\"; ls -l \"$target/microhad\" \"$target/config.json\""
    fi
fi
if { [ -n "$matter_service" ] || [ "$deploy_supervisor" = true ]; } && [ "$activate_paired_only" = false ]; then
    # Stage the selected manifest and supervisor; the service is optional. No launcher activation,
    # credential state transfer, UART ownership change or process restart here.
    ssh_command "dd of=\"$target/.microhad.matter-manifest.new\"" < "$binary.matter-manifest"
    ssh_command "chmod 600 \"$target/.microhad.matter-manifest.new\"; mv \"$target/.microhad.matter-manifest.new\" \"$target/microhad.matter-manifest\""
    if [ -n "$matter_service" ]; then
        ssh_command "dd of=\"$target/.mthread-matter.new\"" < "$matter_service"
        ssh_command "chmod 700 \"$target/.mthread-matter.new\"; mv \"$target/.mthread-matter.new\" \"$target/mthread-matter\""
    fi
    ssh_command "dd of=\"$target/.paired_start.sh.new\"" < "$project_dir/paired_start.sh"
    ssh_command "dd of=\"$target/.paired_control.sh.new\"" < "$project_dir/scripts/paired_control.sh"
    ssh_command "chmod 700 \"$target/.paired_start.sh.new\" \"$target/.paired_control.sh.new\"; mv \"$target/.paired_start.sh.new\" \"$target/paired_start.sh\"; mv \"$target/.paired_control.sh.new\" \"$target/paired_control.sh\""
fi
if [ "$deploy_webui" = true ]; then
    printf 'Uploading web UI to %s:%s/webui\n' "$host" "$target"
    webui_dist="$webui_dir"
    # Free flash before copying new assets while the active index and its
    # referenced bundles remain available if the transfer is interrupted.
    prune_web_bundles temporary
    verify_webui_release "$webui_dist"
    find "$webui_dist" -type f ! -name index.html.gz | while IFS= read -r asset; do
        relative=${asset#"$webui_dist"/}
        case "$relative" in *[!A-Za-z0-9_./-]*|*..*) echo "unsupported Web UI asset path: $relative" >&2; exit 1 ;; esac
        asset_dir=${relative%/*}
        if [ "$asset_dir" = "$relative" ]; then asset_dir=.; fi
        # Keep SSH from consuming the remaining filenames on find's stdin.
        ssh_command "mkdir -p \"$target/webui/$asset_dir\"" < /dev/null
        ssh_command "dd of=\"$target/webui/$relative.new\"" < "$asset"
        ssh_command "chmod 644 \"$target/webui/$relative.new\"; mv \"$target/webui/$relative.new\" \"$target/webui/$relative\"" < /dev/null
    done
    ssh_command "dd of=\"$target/webui/.index.html.gz.new\"" < "$webui_dist/index.html.gz"
    ssh_command "chmod 644 \"$target/webui/.index.html.gz.new\"; mv \"$target/webui/.index.html.gz.new\" \"$target/webui/index.html.gz\"; ls -l \"$target/webui/index.html.gz\""
    prune_web_bundles final
    find "$webui_dist" -type f | while IFS= read -r asset; do
        relative=${asset#"$webui_dist"/}
        ssh_command "test -f \"$target/webui/$relative\"" < /dev/null || exit 1
    done
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
    ssh_command "[ -z \"$rollback_dir\" ] || test -f /tuya/tuya_start.sh.lowjs-backup || cp /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup; chmod 755 \"$target/.gateway_start.sh.new\"; mv \"$target/.gateway_start.sh.new\" /tuya/tuya_start.sh"
fi
if [ "$activate_paired" = true ] || [ "$activate_paired_only" = true ]; then
    ssh_command "dd of=\"$target/.gateway_start.sh.new\"" < "$project_dir/ziggurat_start.sh"
    ssh_command "[ -z \"$rollback_dir\" ] || test -f /tuya/tuya_start.sh.lowjs-backup || cp /tuya/tuya_start.sh /tuya/tuya_start.sh.lowjs-backup; chmod 755 \"$target/.gateway_start.sh.new\"; mv \"$target/.gateway_start.sh.new\" /tuya/tuya_start.sh"
fi
if [ "$restart" = true ]; then
    printf 'Restarting the supervised native daemon\n'
    ssh_command "killall -TERM microhad >/dev/null 2>&1 || true
is_deployed_binary_running() {
    for executable in /proc/[0-9]*/exe; do
        [ -e \"\$executable\" ] || continue
        [ \"\$executable\" -ef \"$target/microhad\" ] && return 0
    done
    return 1
}
retries=0
while [ \$retries -lt 13 ]; do
    if is_deployed_binary_running; then exit 0; fi
    retries=\$((retries + 1))
    sleep 1
done
echo 'ERROR: restart check failed: deployed microhad binary was not running within 13 seconds' >&2
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
