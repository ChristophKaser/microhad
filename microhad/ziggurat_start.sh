#!/bin/sh
# Permanent launcher for the paired native/Ziggurat stack.
legacy=/tuya/native/tuya-zigbee-native
if [ -e "$legacy" ]; then
    for executable in /proc/[0-9]*/exe; do
        [ -e "$executable" ] || continue
        [ "$executable" -ef "$legacy" ] || continue
        echo 'Legacy daemon is still running; refusing to remove its executable' >&2
        exit 1
    done
    rm -f "$legacy" "$legacy.matter-manifest"
fi
export ZIGGURAT_BIN=/tuya/native/ziggurat
export NATIVE_BIN=/tuya/native/microhad
export NATIVE_CONFIG=/tuya/native/config.json
export MAINTENANCE_BIN=/tuya/native/gateway-maintenance
export RUNTIME_CONTROL=/tmp/ziggurat-supervisor
export ZIGGURAT_SOCKET=/tmp/ziggurat/native.sock
export ZIGGURAT_DEVICE=/dev/ttyS1
# Staging the optional service is the explicit opt-in. Matter remains disabled
# on existing installs where this executable is absent.
if [ -x /tuya/native/thread-matter ]; then
    export MATTER_BIN=/tuya/native/thread-matter
fi
/tuya/native/paired_start.sh >/dev/null 2>&1 &
/tuya/set_time.sh &
