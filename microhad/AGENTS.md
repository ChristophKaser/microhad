# Native MicroHAD Gateway

This is a native C++03 gateway for the Tuya MIPS device. Build the target
binary only with `rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129`:

```sh
make host test
make mips
```

`make mips` builds the vendored Mbed TLS dependency and produces the static,
big-endian MIPS binary at `build/mips/microhad-zigbee-native`.

Deploy with `deploy.sh`, passing a local config and password file explicitly.
The permanent device service is `/tuya/tuya_start.sh`; its executable and
configuration live in `/tuya/native`. Never commit device configuration,
credentials, TLS keys, persistent network state, or Zigbee keys.

## Device build, deployment, and checks

The development gateway is `192.168.178.62`. From this directory, use the
untracked password file only by pathname:

```sh
make host test
make mips
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --config /secure/path/config.json --target /tuya/native --activate
```

Deployment atomically replaces the files but deliberately does not restart the
daemon. For an explicitly requested in-device activation, first identify the
current launcher and native leader, then restart only those exact PIDs (the
remaining matching entries are threads and must not be killed separately):

```sh
sshpass -f ../ssh_password.txt ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=yes \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -o PreferredAuthentications=password -o PubkeyAuthentication=no root@192.168.178.62 'ps w'
# Replace LAUNCHER_PID and NATIVE_LEADER_PID with the two PIDs just inspected.
sshpass -f ../ssh_password.txt ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=yes \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -o PreferredAuthentications=password -o PubkeyAuthentication=no root@192.168.178.62 \
  'kill LAUNCHER_PID NATIVE_LEADER_PID; sleep 1; /tuya/tuya_start.sh >/tmp/microhad-native-launch.log 2>&1 &'
```

Check the external dashboard rather than using `curl` on the device (it is not
installed there):

```sh
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/status
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/logs
curl --connect-timeout 5 --max-time 8 -k -sS https://192.168.178.62/api/status
```

The dashboard serves HTTP on port 80 and HTTPS on port 443. The coordinator
thread is the sole owner of the NCP serial connection and `Gateway` state.
Each HTTP listener has an acceptor plus two request workers; all mutations use
the coordinator command queue and status/log reads use its protected snapshots.
The single notification worker owns outbound FCM/browser-push calls and keeps
at most 32 pending jobs in RAM. Do not call `Gateway` directly from a web or
notification thread.

The dashboard's global Push notifications switch persists in
`notifications.json` beneath the runtime directory. Turning it off discards
pending RAM jobs, blocks test and automatic push delivery, and retains browser
subscriptions for a later re-enable. Disabled notifications must be rejected
before notification audit/outbox handling and before the notification-worker
queue, so contact events do not exercise delivery-related code while push is
off.

A contact opening is only an alert candidate. It must remain open for ten
minutes before the first `Window open` notification; closing it cancels that
pending alert. While it remains open, later reminders are sent every five
minutes.

Coordinator logs are intentionally kept in `/tmp/microhad-zigbee-native.log`, a
bounded 512-line RAM-only ring. Permanent radio captures belong under the
ignored `diagnostics/` directory and must never be committed.
