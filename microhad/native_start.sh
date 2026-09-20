#!/bin/sh
# Installed as /tuya/tuya_start.sh by deploy.sh --activate.
# Keep the native coordinator alive after an unexpected exit without reviving
# serialgateway or low.js, which must never share /dev/ttyS1 with it.
while true; do
    /tuya/native/tuya-zigbee-native --config /tuya/native/config.json >>/tuya/native/daemon.log 2>&1
    sleep 2
done &
/tuya/set_time.sh &
