#!/bin/sh
# Source this file, then call workspace_load /absolute/workspace/root.
# workspace.local.env is trusted shell code, not a dotenv data format.
workspace_keys='GATEWAY_IP LAN_INTERFACE TOOLCHAIN LINK_MODE RSDK_ROOT CROSSTOOL_NG_ROOT STOCK_ROOTFS SSH_PASSWORD_FILE SSH_PASSWORD SSH_USER SSH_PORT SSH_CONNECT_TIMEOUT'
workspace_quote() {
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}
workspace_load() {
    workspace_root=$1
    workspace_file=${WORKSPACE_CONFIG-$workspace_root/workspace.local.env}
    case "$workspace_file" in /*) ;; *) workspace_file=$workspace_root/$workspace_file ;; esac
    # Restore pre-existing values after sourcing so the environment wins, even
    # when a value is explicitly empty. Quote data before using shell assignment.
    workspace_saved=
    for workspace_key in $workspace_keys; do
        eval 'workspace_present=${'"$workspace_key"'+x}'
        if [ "$workspace_present" = x ]; then
            eval 'workspace_value=${'"$workspace_key"'}'
            workspace_saved="$workspace_saved
$workspace_key=$(workspace_quote "$workspace_value")"
        fi
    done
    if [ -f "$workspace_file" ] || [ "$workspace_file" = /dev/null ]; then
        sh -n "$workspace_file" 2>/dev/null || { echo 'Invalid shell syntax in workspace configuration' >&2; return 2; }
        . "$workspace_file"
    elif [ "${WORKSPACE_CONFIG+x}" = x ]; then
        echo 'WORKSPACE_CONFIG must name a readable file' >&2
        return 2
    fi
    eval "$workspace_saved"
    : "${GATEWAY_IP=}" "${LAN_INTERFACE=}" "${TOOLCHAIN=rsdk}" "${LINK_MODE=static}"
    workspace_rsdk_default=$( . "$workspace_root/config/toolchains.mk"; printf '%s' "$RSDK_RELATIVE" )
    workspace_crosstool_default=$( . "$workspace_root/config/toolchains.mk"; printf '%s' "$CROSSTOOL_NG_RELATIVE" )
    : "${RSDK_ROOT=$workspace_root/$workspace_rsdk_default}"
    : "${CROSSTOOL_NG_ROOT=$workspace_root/$workspace_crosstool_default}"
    : "${STOCK_ROOTFS=$workspace_root/build/stock-runtime}"
    : "${SSH_PASSWORD_FILE=}" "${SSH_PASSWORD=}" "${SSH_USER=root}" "${SSH_PORT=22}" "${SSH_CONNECT_TIMEOUT=8}"
    case "$TOOLCHAIN" in rsdk|crosstool-ng) ;; *) echo 'TOOLCHAIN must be rsdk or crosstool-ng' >&2; return 2 ;; esac
    case "$SSH_PORT" in ''|*[!0-9]*) echo 'SSH_PORT must be 1..65535' >&2; return 2 ;; esac
    [ "$SSH_PORT" -ge 1 ] && [ "$SSH_PORT" -le 65535 ] || { echo 'SSH_PORT must be 1..65535' >&2; return 2; }
    case "$SSH_CONNECT_TIMEOUT" in ''|*[!0-9]*|0) echo 'SSH_CONNECT_TIMEOUT must be positive seconds' >&2; return 2 ;; esac
    for workspace_key in RSDK_ROOT CROSSTOOL_NG_ROOT STOCK_ROOTFS SSH_PASSWORD_FILE; do
        eval 'workspace_value=${'"$workspace_key"'}'
        case "$workspace_value" in ''|/*) ;; *) workspace_value=$workspace_root/$workspace_value ;; esac
        if [ -n "$workspace_value" ]; then
            workspace_value=$(realpath -m -- "$workspace_value") || return
        fi
        eval "$workspace_key=$(workspace_quote "$workspace_value")"
    done
    # Password stays a shell variable; do not export it to build subprocesses.
    export GATEWAY_IP LAN_INTERFACE TOOLCHAIN LINK_MODE RSDK_ROOT CROSSTOOL_NG_ROOT STOCK_ROOTFS
    export SSH_PASSWORD_FILE SSH_USER SSH_PORT SSH_CONNECT_TIMEOUT
    SSH_PASSWORD_SET=
    [ -z "$SSH_PASSWORD" ] || SSH_PASSWORD_SET=1
}

# Also provide a small NUL-delimited bridge for Python and Node. Never emit the
# password; the SSH wrapper reads it directly from the sourced file.
case "$0" in
    */workspace-env.sh|workspace-env.sh)
        workspace_script_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
        workspace_load "$workspace_script_root" || exit $?
        case "${1-}" in
            --null)
                for workspace_key in $workspace_keys SSH_PASSWORD_SET; do
                    [ "$workspace_key" != SSH_PASSWORD ] || continue
                    eval 'workspace_value=${'"$workspace_key"'}'
                    printf '%s\000%s\000' "$workspace_key" "$workspace_value"
                done ;;
            --get)
                case " $workspace_keys " in *" ${2-} "*) ;; *) exit 2 ;; esac
                [ "${2-}" != SSH_PASSWORD ] || exit 2
                eval 'workspace_value=${'"$2"'}'
                printf '%s\n' "$workspace_value" ;;
            *) echo 'usage: sh scripts/workspace-env.sh --null | --get KEY' >&2; exit 2 ;;
        esac ;;
esac
