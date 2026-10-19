#!/usr/bin/env bash
set -euo pipefail
# Host launcher passes these explicitly; the private workspace file stays on host.
: "${GATEWAY:?Set GATEWAY from host GATEWAY_IP}" "${BACKBONE_INTERFACE:?Set BACKBONE_INTERFACE from host LAN_INTERFACE}"
relay_pid=
agent_pid=
cleanup() {
    for child in "$agent_pid" "$relay_pid"; do
        if [[ -n "$child" ]]; then
            kill "$child" 2>/dev/null || true
            wait "$child" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT
trap 'exit 0' TERM INT
# One client only; never invoke a physical RCP reset or change its channel.
socat -d -d pty,link=/tmp/ttyOTBR,raw,echo=0,wait-slave \
    "tcp:${GATEWAY}:${THREAD_PORT:-9998},nodelay,keepalive,connect-timeout=5" &
relay_pid=$!
for attempt in {1..50}; do
    [[ ! -e /tmp/ttyOTBR ]] || break
    kill -0 "$relay_pid"
    sleep 0.1
done
/opt/otbr-agent -I "${THREAD_INTERFACE:-wpan-otbr-test}" -B "${BACKBONE_INTERFACE}" \
    -d7 -v -s --auto-attach=0 --data-path /var/lib/thread \
    --vendor-name Diagnostic --model-name Ziggurat-TCP \
    'spinel+hdlc+uart:///tmp/ttyOTBR?uart-baudrate=460800&no-reset&attach-to-running-rcp' &
agent_pid=$!
wait "$agent_pid"
