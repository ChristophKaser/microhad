#!/bin/sh
# Installed as /tuya/tuya_start.sh by deploy.sh --activate.
# Keep the native coordinator alive after an unexpected exit without reviving
# serialgateway or low.js, which must never share /dev/ttyS1 with it.
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
while true; do
    /tuya/native/microhad --config /tuya/native/config.json >>/tmp/microhad-daemon.log 2>&1
    sleep 2
done &
/tuya/set_time.sh &
