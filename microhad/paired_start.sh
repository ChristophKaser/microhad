#!/bin/sh
# Foreground paired supervisor. Not installed/activated by this task.
# Required explicit absolute paths: ZIGGURAT_BIN, NATIVE_BIN, NATIVE_CONFIG,
# MAINTENANCE_BIN. RUNTIME_CONTROL defaults to a private RAM directory.
set -u
umask 077
: "${ZIGGURAT_BIN:?}" "${NATIVE_BIN:?}" "${NATIVE_CONFIG:?}" "${MAINTENANCE_BIN:?}"
control=${RUNTIME_CONTROL:-/tmp/ziggurat-supervisor}
socket_path=${ZIGGURAT_SOCKET:-/tmp/ziggurat/native.sock}
device=${ZIGGURAT_DEVICE:-/dev/ttyS1}
zig_log=${ZIGGURAT_LOG:-/dev/null}
zig_log_level=${ZIGGURAT_LOG_LEVEL:-info}
seed_helper=${RNG_SEED_HELPER:-/tuya/native/rng-seed}
seed_file=${RNG_SEED_FILE:-/tuya/native/ziggurat-rng.seed}
seed_log=${RNG_SEED_LOG:-/tmp/ziggurat-rng-seed.log}
limit=${RESTART_LIMIT:-5}
startup_limit=${STARTUP_LIMIT:-15}
stop_limit=${STOP_LIMIT:-8}
backoff=${RESTART_BACKOFF:-3}
matter_bin=${MATTER_BIN:-}
matter_state=${MATTER_STATE_DIR:-/tuya/native/matter}
matter_socket=${MATTER_SOCKET:-/tmp/microhad-matter.sock}
thread_socket=${THREAD_SOCKET:-/tmp/ziggurat/thread.sock}
matter_limit=${MATTER_RESTART_LIMIT:-5}
matter_log_level=${MATTER_LOG_LEVEL:-info}
matter_level_restart=false
syslogd=${SYSLOGD_BIN:-/bin/busybox}
case "$limit:$startup_limit:$stop_limit:$backoff:$matter_limit" in *[!0-9:]*|:*|*::*) exit 2;; esac
case "$zig_log_level" in off|error|warn|info|debug|trace) ;; *) echo 'Invalid ZIGGURAT_LOG_LEVEL' >&2;exit 2;; esac
case "$matter_log_level" in off|error|warn|info|debug|trace) ;; *) echo 'Invalid MATTER_LOG_LEVEL' >&2;exit 2;; esac
for path in "$ZIGGURAT_BIN" "$NATIVE_BIN" "$NATIVE_CONFIG" "$MAINTENANCE_BIN" "$control" "$socket_path" "$device" "$zig_log" "$seed_helper" "$seed_file" "$seed_log"; do
    case "$path" in /*) ;; *) echo 'Absolute paths required' >&2;exit 2;; esac
done
if [ -n "$matter_bin" ]; then
    for path in "$matter_bin" "$matter_state" "$matter_socket" "$thread_socket"; do
        case "$path" in /*) ;; *) echo 'Absolute Matter paths required' >&2;exit 2;; esac
    done
    [ -x "$matter_bin" ] || { echo 'Matter service executable missing' >&2;exit 2; }
    [ "$thread_socket" != "$socket_path" ] && [ "$matter_socket" != "$socket_path" ] &&
        [ "$matter_socket" != "$thread_socket" ] || { echo 'Distinct IPC paths required' >&2;exit 2; }
fi
# Atomic directory claim refuses stale/duplicate supervisors; operator inspects it.
mkdir "$control" || exit 2
printf '%s\n' "$$" > "$control/supervisor.pid"
: > "$zig_log" 2>/dev/null || true
: > "$seed_log"
# Stock firmware's BusyBox syslogd owns the 512 KiB IPC ring and forwards to
# the configured collector. Replace its prior instance to apply these flags.
[ -x "$syslogd" ] || { echo 'BusyBox syslogd executable missing' >&2; exit 2; }
for old_syslog_pid in $($syslogd ps 2>/dev/null | awk '$5 == "/bin/busybox" && $6 == "syslogd" {print $1}'); do
    kill "$old_syslog_pid" 2>/dev/null || true
done
sleep 1
$syslogd syslogd -C512 -L -R 192.168.178.51:7878 -m 0
sleep 1
$syslogd ps 2>/dev/null | awk '$5 == "/bin/busybox" && $6 == "syslogd" && $7 == "-C512" && $8 == "-L" && $9 == "-R" && $10 == "192.168.178.51:7878" {found=1} END {exit !found}' || {
    echo 'BusyBox syslogd did not start with the required ring and remote logging options' >&2
    rm -f "$control/supervisor.pid"
    rm -rf "$control"
    exit 2
}
if [ -x "$seed_helper" ]; then
    "$seed_helper" restore "$seed_file" >>"$seed_log" 2>&1 || true
    (
        sleep 180
        "$seed_helper" save "$seed_file"
    ) >>"$seed_log" 2>&1 </dev/null &
else
    echo 'RNG seed helper missing; cold-boot entropy wait may delay Ziggurat' >>"$seed_log"
fi
zig_pid=;native_pid=;matter_pid=;retries=0;matter_retries=0;matter_next=0;quiesced=false
stop_child() {
    child=$1
    [ -n "$child" ] || return 0
    kill -TERM "$child" 2>/dev/null || true
    elapsed=0
    while kill -0 "$child" 2>/dev/null && [ "$elapsed" -lt "$stop_limit" ]; do
        sleep 1;elapsed=$((elapsed+1))
    done
    if kill -0 "$child" 2>/dev/null; then
        echo 'Child stop timed out; final freeze evidence not certified' >&2
        kill -KILL "$child" 2>/dev/null || true
    fi
    wait "$child" 2>/dev/null || true
}
cleanup() {
    trap '' TERM INT HUP
    trap - EXIT
    stop_child "$matter_pid";matter_pid=
    stop_child "$native_pid";native_pid=
    stop_child "$zig_pid";zig_pid=
    rm -f "$control/matter.pid" "$control/matter.failed" "$control/active-matter-log-level" "$control/requested-matter-log-level" "$control/requested-matter-log-level."* "$control/native.pid" "$control/ziggurat.pid" "$control/supervisor.pid" "$control/quiesce" "$control/quiesced" "$control/stop" "$control/active-log-level" "$control/requested-log-level" "$control/requested-log-level."*
    # Device lacks rmdir. Only remove this invocation's known control directory.
    rm -rf "$control"
}
trap 'cleanup;exit 0' TERM INT HUP
trap cleanup EXIT
prepare_socket_path() {
    path=$1
    [ -e "$path" ] || [ -L "$path" ] || return 0
    [ ! -L "$path" ] && [ -S "$path" ] || {
        echo "IPC path exists but is not a socket; refusing to remove it: $path" >&2
        return 1
    }
    [ -r /proc/net/unix ] || {
        echo "Cannot inspect active Unix sockets; refusing to remove: $path" >&2
        return 1
    }
    active=$(awk -v socket_path="$path" '$NF == socket_path {found=1} END {print found+0}' /proc/net/unix) || return 1
    [ "$active" = 0 ] || {
        echo "Unix socket is still registered in /proc/net/unix; refusing to remove: $path" >&2
        return 1
    }
    rm -f "$path" || return 1
}
start_zig() {
    "$MAINTENANCE_BIN" check-free "$device" || { echo 'UART already owned or cannot be inspected' >&2;return 1; }
    prepare_socket_path "$socket_path" || return 1
    if [ -n "$matter_bin" ]; then
        prepare_socket_path "$thread_socket" || return 1
    fi
    # Socket parent must be private; stale paths were removed only after the
    # kernel socket table confirmed no process still owns them.
    parent=${socket_path%/*}
    if [ ! -d "$parent" ]; then mkdir "$parent" || return 1;chmod 700 "$parent";fi
    set --
    if [ -n "$matter_bin" ]; then
        parent=${thread_socket%/*}
        if [ ! -d "$parent" ]; then mkdir "$parent" || return 1;chmod 700 "$parent";fi
        set -- --thread-socket "$thread_socket"
    fi
    NO_COLOR=1 "$ZIGGURAT_BIN" --device "$device" --baudrate 460800 --flow-control hardware \
        --spinel-iid 1 --api cobs-unix --listen "unix:$socket_path" --log-level "$zig_log_level" "$@" 2>&1 &
    zig_pid=$!;printf '%s\n' "$zig_pid" > "$control/ziggurat.pid"
    deadline=$(($(date +%s)+startup_limit))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ ! -e "$control/stop" ] || return 1
        kill -0 "$zig_pid" 2>/dev/null || return 1
        if "$MAINTENANCE_BIN" cobs-hello "$socket_path" >/dev/null 2>&1; then
            printf '%s\n' "$zig_log_level" > "$control/active-log-level"
            return 0
        fi
        sleep 1
    done
    echo 'API readiness deadline exceeded' >&2;return 1
}
start_native() {
    "$NATIVE_BIN" --config "$NATIVE_CONFIG" >/dev/null 2>&1 &
    native_pid=$!;printf '%s\n' "$native_pid" > "$control/native.pid"
}
poll_matter() {
    [ -n "$matter_bin" ] || return 0
    if [ -f "$control/requested-matter-log-level" ]; then
        requested_matter_level=
        IFS= read -r requested_matter_level < "$control/requested-matter-log-level" || requested_matter_level=
        case "$requested_matter_level" in
            off|error|warn|info|debug|trace)
                if [ "$requested_matter_level" != "$matter_log_level" ] &&
                    [ -n "$matter_pid" ]; then
                    matter_log_level=$requested_matter_level
                    stop_child "$matter_pid";matter_pid=
                    rm -f "$control/matter.pid" "$control/active-matter-log-level"
                    matter_level_restart=true
                    matter_next=0
                fi ;;
        esac
        rm -f "$control/requested-matter-log-level"
    fi
    [ ! -e "$control/quiesce" ] && [ ! -e "$control/stop" ] || return 0
    if [ -n "$matter_pid" ] && kill -0 "$matter_pid" 2>/dev/null; then return 0;fi
    if [ -n "$matter_pid" ]; then
        wait "$matter_pid" 2>/dev/null || true;matter_pid=
        rm -f "$control/matter.pid" "$control/active-matter-log-level"
        matter_next=$(($(date +%s)+backoff))
    fi
    if [ "$matter_retries" -ge "$matter_limit" ] && [ "$matter_level_restart" = false ]; then
        if [ ! -e "$control/matter.failed" ]; then
            echo 'Matter restart budget exhausted; Zigbee remains running' >&2
            : > "$control/matter.failed"
        fi
        return 0
    fi
    [ "$(date +%s)" -ge "$matter_next" ] || return 0
    # A missing Thread endpoint must not prevent the native bridge from running.
    [ -S "$thread_socket" ] || return 0
    prepare_socket_path "$matter_socket" || { matter_next=$(($(date +%s)+backoff));return 0; }
    "$matter_bin" --socket "$matter_socket" --state-dir "$matter_state" \
        --radio-url "spinel+hdlc+unix://$thread_socket?no-reset" --log-level "$matter_log_level" 2>&1 &
    matter_pid=$!
    if [ "$matter_level_restart" = false ]; then matter_retries=$((matter_retries+1));fi
    matter_level_restart=false
    printf '%s\n' "$matter_log_level" > "$control/active-matter-log-level"
    printf '%s\n' "$matter_pid" > "$control/matter.pid"
}
while :; do
    [ ! -e "$control/stop" ] || exit 0
    if [ -e "$control/quiesce" ]; then
        if [ "$quiesced" = false ]; then
            stop_child "$matter_pid";matter_pid=
            stop_child "$native_pid";native_pid=
            rm -f "$control/matter.pid" "$control/active-matter-log-level" "$control/native.pid";: > "$control/quiesced";quiesced=true
        fi
        # Leave Ziggurat alive for the host freeze/export tool. No respawn.
        sleep 1;continue
    fi
    if [ "$quiesced" = true ]; then
        echo 'Quiesced state requires explicit stop and fresh supervisor start' >&2
        exit 1
    fi
    if [ -z "$zig_pid" ] || ! kill -0 "$zig_pid" 2>/dev/null; then
        requested_level=
        if [ -f "$control/requested-log-level" ]; then
            IFS= read -r requested_level < "$control/requested-log-level" || requested_level=
        fi
        case "$requested_level" in off|error|warn|info|debug|trace) ;; *) requested_level= ;; esac
        level_change=false
        if [ -n "$requested_level" ] && [ "$requested_level" != "$zig_log_level" ]; then
            zig_log_level=$requested_level
            level_change=true
        fi
        if [ "$level_change" = true ] && [ -n "$native_pid" ] && kill -0 "$native_pid" 2>/dev/null; then
            # A requested level change deliberately recycles only Ziggurat.
            zig_pid=
            if ! start_zig; then
                stop_child "$zig_pid";zig_pid=
                rm -f "$control/requested-log-level"
                sleep "$backoff";continue
            fi
            rm -f "$control/requested-log-level"
        else
            # Unexpected radio failures retain the existing pair recovery order.
            stop_child "$matter_pid";matter_pid=
            rm -f "$control/matter.pid" "$control/active-matter-log-level"
            stop_child "$native_pid";native_pid=
            stop_child "$zig_pid";zig_pid=
            rm -f "$control/requested-log-level"
            if [ "$retries" -ge "$limit" ]; then echo 'Restart budget exhausted' >&2;exit 1;fi
            retries=$((retries+1))
            if ! start_zig; then stop_child "$zig_pid";zig_pid=;sleep "$backoff";continue;fi
            # A concurrent maintenance request must prevent client startup.
            [ ! -e "$control/quiesce" ] && [ ! -e "$control/stop" ] || continue
            start_native
        fi
    elif [ -z "$native_pid" ] || ! kill -0 "$native_pid" 2>/dev/null; then
        stop_child "$native_pid";native_pid=
        if [ "$retries" -ge "$limit" ]; then echo 'Restart budget exhausted' >&2;exit 1;fi
        retries=$((retries+1));sleep "$backoff"
        [ ! -e "$control/quiesce" ] && [ ! -e "$control/stop" ] || continue
        start_native
    fi
    poll_matter
    sleep 1
done
