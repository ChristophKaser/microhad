#!/bin/sh
# Candidate permanent launcher; not installed or activated by this task.
export ZIGGURAT_BIN=/tuya/native/ziggurat
export NATIVE_BIN=/tuya/native/tuya-zigbee-native
export NATIVE_CONFIG=/tuya/native/config.json
export MAINTENANCE_BIN=/tuya/native/gateway-maintenance
export RUNTIME_CONTROL=/tmp/ziggurat-supervisor
export ZIGGURAT_SOCKET=/tmp/ziggurat/native.sock
export ZIGGURAT_DEVICE=/dev/ttyS1
/tuya/native/paired_start.sh >/dev/null 2>&1 &
/tuya/set_time.sh &
