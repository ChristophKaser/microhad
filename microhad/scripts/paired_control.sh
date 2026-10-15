#!/bin/sh
# Local gateway control. quiesce keeps Ziggurat alive for host freeze/export.
# stop requires operator confirmation of a saved frozen snapshot. No broad kills.
set -u
control=${RUNTIME_CONTROL:-/tmp/ziggurat-supervisor}
case "${1:-}" in
 log-level|raw-frames|radio-trace)
    level=${2:-}
    if [ "$1" = raw-frames ] || [ "$1" = radio-trace ]; then
        case "$level" in on|off) ;; *) echo "usage: paired_control.sh $1 on|off" >&2;exit 2;; esac
        setting=$( [ "$1" = radio-trace ] && printf radio-trace || printf raw-frames )
        label=$( [ "$1" = radio-trace ] && printf 'radio trace' || printf 'raw frames' )
    else
        case "$level" in off|error|warn|info|debug|trace) ;; *) echo 'usage: paired_control.sh log-level off|error|warn|info|debug|trace' >&2;exit 2;; esac
        setting=log-level
        label='log level'
    fi
    [ -d "$control" ] && [ -f "$control/supervisor.pid" ] &&
        [ -f "$control/ziggurat.pid" ] && [ -f "$control/active-$setting" ] &&
        [ ! -e "$control/quiesce" ] && [ ! -e "$control/stop" ] || {
        echo 'Paired supervisor or Ziggurat is not running' >&2;exit 1;
    }
    supervisor_pid=$(cat "$control/supervisor.pid") || exit 1
    ziggurat_pid=$(cat "$control/ziggurat.pid") || exit 1
    case "$supervisor_pid:$ziggurat_pid" in *[!0-9:]*|:*|*::*) echo 'Invalid supervisor process state' >&2;exit 1;; esac
    [ "$supervisor_pid" -gt 1 ] && [ "$ziggurat_pid" -gt 1 ] || { echo 'Invalid supervisor process state' >&2;exit 1; }
    kill -0 "$supervisor_pid" 2>/dev/null && kill -0 "$ziggurat_pid" 2>/dev/null || {
        echo 'Paired supervisor or Ziggurat is not running' >&2;exit 1;
    }
    current=$(cat "$control/active-$setting") || exit 1
    if [ "$current" = "$level" ]; then echo "Ziggurat is already using $level";exit 0;fi
    umask 077
    staged="$control/requested-$setting.$$"
    trap 'rm -f "$staged"' EXIT HUP INT TERM
    printf '%s\n' "$level" > "$staged" || exit 1
    mv "$staged" "$control/requested-$setting" || exit 1
    kill -TERM "$ziggurat_pid" 2>/dev/null || {
        rm -f "$control/requested-$setting"
        echo 'Could not signal Ziggurat' >&2;exit 1;
    }
    trap - EXIT HUP INT TERM
    echo "Requested Ziggurat $label $level; restarting Ziggurat" ;;
 matter-log-level|matter-raw-frames)
    level=${2:-}
    if [ "$1" = matter-raw-frames ]; then
        case "$level" in on|off) ;; *) echo 'usage: paired_control.sh matter-raw-frames on|off' >&2;exit 2;; esac
        setting=matter-raw-frames
        label='raw frames'
    else
        case "$level" in off|error|warn|info|debug|trace) ;; *) echo 'usage: paired_control.sh matter-log-level off|error|warn|info|debug|trace' >&2;exit 2;; esac
        setting=matter-log-level
        label='log level'
    fi
    [ -d "$control" ] && [ -f "$control/supervisor.pid" ] &&
        [ -f "$control/matter.pid" ] && [ -f "$control/active-$setting" ] &&
        [ ! -e "$control/quiesce" ] && [ ! -e "$control/stop" ] || {
        echo 'Paired supervisor or Matter is not running' >&2;exit 1;
    }
    supervisor_pid=$(cat "$control/supervisor.pid") || exit 1
    matter_pid=$(cat "$control/matter.pid") || exit 1
    case "$supervisor_pid:$matter_pid" in *[!0-9:]*|:*|*::*) echo 'Invalid supervisor process state' >&2;exit 1;; esac
    [ "$supervisor_pid" -gt 1 ] && [ "$matter_pid" -gt 1 ] || { echo 'Invalid supervisor process state' >&2;exit 1; }
    kill -0 "$supervisor_pid" 2>/dev/null && kill -0 "$matter_pid" 2>/dev/null || {
        echo 'Paired supervisor or Matter is not running' >&2;exit 1;
    }
    current=$(cat "$control/active-$setting") || exit 1
    if [ "$current" = "$level" ]; then echo "Matter is already using $level";exit 0;fi
    umask 077
    staged="$control/requested-$setting.$$"
    trap 'rm -f "$staged"' EXIT HUP INT TERM
    printf '%s\n' "$level" > "$staged" || exit 1
    mv "$staged" "$control/requested-$setting" || exit 1
    kill -TERM "$matter_pid" 2>/dev/null || {
        rm -f "$control/requested-$setting"
        echo 'Could not signal Matter' >&2;exit 1;
    }
    trap - EXIT HUP INT TERM
    echo "Requested Matter $label $level; restarting Matter" ;;
 quiesce)
    [ -f "$control/supervisor.pid" ] || exit 1
    : > "$control/quiesce"
    deadline=0
    while [ ! -f "$control/quiesced" ] && [ "$deadline" -lt 15 ]; do sleep 1;deadline=$((deadline+1));done
    [ -f "$control/quiesced" ] || exit 1
    echo 'Native quiesced; Ziggurat retained. Freeze/export before stop.' ;;
 stop)
    [ "${2:-}" = --frozen-snapshot-saved ] && [ -f "$control/quiesced" ] || { echo 'Quiesce and save frozen snapshot first' >&2;exit 2; }
    : > "$control/stop" ;;
 *) echo 'usage: paired_control.sh log-level LEVEL | raw-frames on|off | radio-trace on|off | matter-log-level LEVEL | matter-raw-frames on|off | quiesce | stop --frozen-snapshot-saved' >&2;exit 2;;
esac
