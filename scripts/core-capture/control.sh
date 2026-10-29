#!/bin/sh
# Installed next to gateway-core and core-capture.conf (IPv4 PORT).
set -eu
umask 077
base=$(CDPATH= cd "${0%/*}" && pwd)
state=/tmp/gateway-core-capture
pattern="|$base/gateway-core %p %s %t"
case "${1:-}" in
 start)
    [ -s "$base/core-capture.conf" ] || exit 0
    [ -x "$base/gateway-core" ] || exit 1
    # core_pattern alone does not imply ELF core dumping was compiled in.
    if ! awk '$3 ~ /^elf_core_dump(\.|$)/ {found=1} END {exit !found}' /proc/kallsyms; then
        echo 'TCP core capture inactive: kernel has no ELF core-dump callback' >&2
        exit 1
    fi
    [ "${#pattern}" -lt 128 ] || exit 1
    mkdir -p "$state"
    [ -d "$state" ] && [ ! -L "$state" ] || exit 1
    chmod 700 "$state"
    if [ ! -f "$state/previous-pattern" ]; then
        cat /proc/sys/kernel/core_pattern > "$state/previous-pattern"
        cat /proc/sys/kernel/core_pipe_limit > "$state/previous-limit"
    fi
    printf '1\n' > /proc/sys/kernel/core_pipe_limit
    printf '%s\n' "$pattern" > /proc/sys/kernel/core_pattern
    if [ -f "$state/retry.pid" ] && kill -0 "$(cat "$state/retry.pid")" 2>/dev/null; then exit 0; fi
    "$0" retry-loop </dev/null >/dev/null 2>&1 &
    echo "$!" > "$state/retry.pid"
    ;;
 retry-loop)
    sleep 1
    while [ "$(cat "$state/retry.pid" 2>/dev/null || true)" = "$$" ] &&
          [ "$(cat /proc/sys/kernel/core_pattern)" = "$pattern" ]; do
        "$base/gateway-core" --retry || true
        sleep 30
    done
    ;;
 stop)
    if [ "$(cat /proc/sys/kernel/core_pattern)" = "$pattern" ]; then
        cat "$state/previous-pattern" > /proc/sys/kernel/core_pattern
        cat "$state/previous-limit" > /proc/sys/kernel/core_pipe_limit
    fi
    # Loop exits on its next wake; avoid killing an unverified reused PID.
    rm -f "$state/previous-pattern" "$state/previous-limit" "$state/retry.pid"
    ;;
 *) echo 'usage: core-capture-control.sh start|stop' >&2; exit 2 ;;
esac
