#!/bin/sh
# Run through SSH stdin with the explicitly chosen prototype path as $1.
# No service control, radio access, or persistent state writes.
set -u
ulimit -c 0 || exit 1
test "$#" -eq 1 || exit 2
case "$1" in
    /tmp/ziggurat-rust-prototype-*) ;;
    *) echo "Refusing a path outside the temporary prototype namespace" >&2; exit 2 ;;
esac
test -x "$1" || exit 2
"$1" &
prototype_pid=$!
(
    sleep 15
    echo "Prototype exceeded 15 seconds; terminating PID $prototype_pid" >&2
    kill -9 "$prototype_pid" 2>/dev/null
) &
watchdog_pid=$!
trap 'kill "$watchdog_pid" "$prototype_pid" 2>/dev/null' HUP INT TERM
wait "$prototype_pid"
prototype_status=$?
kill "$watchdog_pid" 2>/dev/null
wait "$watchdog_pid" 2>/dev/null
echo "Prototype exit status: $prototype_status"
exit "$prototype_status"
