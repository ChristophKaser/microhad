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
zig_log=${ZIGGURAT_LOG:-/tmp/ziggurat.log}
limit=${RESTART_LIMIT:-5}
startup_limit=${STARTUP_LIMIT:-15}
stop_limit=${STOP_LIMIT:-8}
backoff=${RESTART_BACKOFF:-3}
matter_bin=${MATTER_BIN:-}
matter_state=${MATTER_STATE_DIR:-/tuya/native/matter}
matter_socket=${MATTER_SOCKET:-/tmp/microhad-matter.sock}
thread_socket=${THREAD_SOCKET:-/tmp/ziggurat/thread.sock}
matter_limit=${MATTER_RESTART_LIMIT:-5}
case "$limit:$startup_limit:$stop_limit:$backoff:$matter_limit" in *[!0-9:]*|:*|*::*) exit 2;; esac
for path in "$ZIGGURAT_BIN" "$NATIVE_BIN" "$NATIVE_CONFIG" "$MAINTENANCE_BIN" "$control" "$socket_path" "$device" "$zig_log"; do
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
: > "$zig_log"
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
    rm -f "$control/matter.pid" "$control/matter.failed" "$control/native.pid" "$control/ziggurat.pid" "$control/supervisor.pid" "$control/quiesce" "$control/quiesced" "$control/stop"
    # Device lacks rmdir. Only remove this invocation's known control directory.
    rm -rf "$control"
}
trap 'cleanup;exit 0' TERM INT HUP
trap cleanup EXIT
start_zig() {
    "$MAINTENANCE_BIN" check-free "$device" || { echo 'UART already owned or cannot be inspected' >&2;return 1; }
    # Socket parent must be private; Ziggurat validates stale/live socket itself.
    parent=${socket_path%/*}
    if [ ! -d "$parent" ]; then mkdir "$parent" || return 1;chmod 700 "$parent";fi
    set --
    if [ -n "$matter_bin" ]; then
        parent=${thread_socket%/*}
        if [ ! -d "$parent" ]; then mkdir "$parent" || return 1;chmod 700 "$parent";fi
        set -- --thread-socket "$thread_socket"
    fi
    "$ZIGGURAT_BIN" --device "$device" --baudrate 460800 --flow-control hardware \
        --spinel-iid 1 --api cobs-unix --listen "unix:$socket_path" --log-level info "$@" >>"$zig_log" 2>&1 &
    zig_pid=$!;printf '%s\n' "$zig_pid" > "$control/ziggurat.pid"
    deadline=$(($(date +%s)+startup_limit))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ ! -e "$control/stop" ] || return 1
        kill -0 "$zig_pid" 2>/dev/null || return 1
        if "$MAINTENANCE_BIN" cobs-hello "$socket_path" >/dev/null 2>&1; then return 0;fi
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
    if [ -n "$matter_pid" ] && kill -0 "$matter_pid" 2>/dev/null; then return 0;fi
    if [ -n "$matter_pid" ]; then
        wait "$matter_pid" 2>/dev/null || true;matter_pid=
        rm -f "$control/matter.pid"
        matter_next=$(($(date +%s)+backoff))
    fi
    if [ "$matter_retries" -ge "$matter_limit" ]; then
        if [ ! -e "$control/matter.failed" ]; then
            echo 'Matter restart budget exhausted; Zigbee remains running' >&2
            : > "$control/matter.failed"
        fi
        return 0
    fi
    [ "$(date +%s)" -ge "$matter_next" ] || return 0
    # A missing Thread endpoint must not prevent the native bridge from running.
    [ -S "$thread_socket" ] || return 0
    "$matter_bin" --socket "$matter_socket" --state-dir "$matter_state" \
        --radio-url "spinel+hdlc+unix://$thread_socket?no-reset" >/dev/null 2>&1 &
    matter_pid=$!;matter_retries=$((matter_retries+1))
    printf '%s\n' "$matter_pid" > "$control/matter.pid"
}
while :; do
    [ ! -e "$control/stop" ] || exit 0
    if [ -e "$control/quiesce" ]; then
        if [ "$quiesced" = false ]; then
            stop_child "$matter_pid";matter_pid=
            stop_child "$native_pid";native_pid=
            rm -f "$control/matter.pid" "$control/native.pid";: > "$control/quiesced";quiesced=true
        fi
        # Leave Ziggurat alive for the host freeze/export tool. No respawn.
        sleep 1;continue
    fi
    if [ "$quiesced" = true ]; then
        echo 'Quiesced state requires explicit stop and fresh supervisor start' >&2
        exit 1
    fi
    if [ -z "$zig_pid" ] || ! kill -0 "$zig_pid" 2>/dev/null; then
        stop_child "$matter_pid";matter_pid=
        rm -f "$control/matter.pid"
        stop_child "$native_pid";native_pid=
        stop_child "$zig_pid";zig_pid=
        if [ "$retries" -ge "$limit" ]; then echo 'Restart budget exhausted' >&2;exit 1;fi
        retries=$((retries+1))
        if ! start_zig; then stop_child "$zig_pid";zig_pid=;sleep "$backoff";continue;fi
        # A concurrent maintenance request must prevent client startup.
        [ ! -e "$control/quiesce" ] && [ ! -e "$control/stop" ] || continue
        start_native
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
