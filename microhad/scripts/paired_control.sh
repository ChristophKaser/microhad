#!/bin/sh
# Local gateway control. quiesce keeps Ziggurat alive for host freeze/export.
# stop requires operator confirmation of a saved frozen snapshot. No broad kills.
set -u
control=${RUNTIME_CONTROL:-/tmp/ziggurat-supervisor}
case "${1:-}" in
 quiesce)
    [ -f "$control/supervisor.pid" ] || exit 1
    touch "$control/quiesce"
    deadline=0
    while [ ! -f "$control/quiesced" ] && [ "$deadline" -lt 15 ]; do sleep 1;deadline=$((deadline+1));done
    [ -f "$control/quiesced" ] || exit 1
    echo 'Native quiesced; Ziggurat retained. Freeze/export before stop.' ;;
 stop)
    [ "${2:-}" = --frozen-snapshot-saved ] && [ -f "$control/quiesced" ] || { echo 'Quiesce and save frozen snapshot first' >&2;exit 2; }
    touch "$control/stop" ;;
 *) echo 'usage: paired_control.sh quiesce | stop --frozen-snapshot-saved' >&2;exit 2;;
esac
