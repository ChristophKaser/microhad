#!/bin/sh
# Safe stock-gateway smoke test: no clients and no physical radio transport.
set -u
ulimit -c 0 || exit 1
test "$#" -eq 1 || exit 2
case "$1" in
    /tmp/ziggurat-server-smoke-*) ;;
    *) exit 2 ;;
esac
binary=$1
test -x "$binary" || exit 2
bounded_cli() {
    "$binary" "$1" &
    child=$!
    (sleep 10; kill -9 "$child" 2>/dev/null) &
    watchdog=$!
    wait "$child"
    result=$?
    kill "$watchdog" 2>/dev/null
    wait "$watchdog" 2>/dev/null
    return "$result"
}
bounded_cli --version || exit 1
bounded_cli --help || exit 1
test ! -e "$binary.log" || exit 2
"$binary" --device tcp://127.0.0.1:9 --listen 127.0.0.1:0 --log-level info > "$binary.log" 2>&1 &
child=$!
(sleep 10; kill -9 "$child" 2>/dev/null) &
watchdog=$!
trap 'kill "$child" "$watchdog" 2>/dev/null' HUP INT TERM EXIT
sleep 3
cat "$binary.log"
kill -0 "$child" 2>/dev/null || { wait "$child"; exit 1; }
awk '/^(Name|State|VmSize|VmRSS|Threads):/' "/proc/$child/status"
kill "$child"
wait "$child" 2>/dev/null
kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null
trap - HUP INT TERM EXIT
echo "PASS CLI and three-second localhost-only server startup; no radio client connected."
