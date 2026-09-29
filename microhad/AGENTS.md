# Native MicroHAD Gateway

This is a native C++03 gateway for the Tuya MIPS device. Build the target
binary only with `rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129`:

```sh
make host test
make mips
```

`make mips` builds the vendored Mbed TLS dependency and produces the static,
big-endian MIPS binary at `build/mips/microhad-zigbee-native`.

Deploy with `deploy.sh` and the password file explicitly. Deployment updates
both the binary and web UI by default and preserves the device configuration;
pass `--binary-only`, `--webui-only`, or `--both` to select assets, and pass
`--config` only when intentionally replacing configuration. `--config`,
`--activate`, and `--restart` require a binary deployment. The permanent device service is
`/tuya/tuya_start.sh`; its executable and configuration live in `/tuya/native`.
Never commit device configuration, credentials, production TLS keys,
persistent network state, or Zigbee keys. The only exception is the
non-production localhost key at `test/fixtures/tls/localhost.key`, which is
used exclusively by host integration tests. It must never be installed,
packaged, or referenced by deployment code.

## Device build, deployment, and checks

The development gateway is `192.168.178.62`. From this directory, use the
untracked password file only by pathname:

```sh
make host test
make mips
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native --restart
```

Service restarts are allowed when needed to load a deployed binary, unless the
restart is expected to be destructive. `--restart` is only allowed for
`/tuya/native`; it signals the exact `microhad-zigbee-native` daemon and the
permanent launcher restarts it from the newly deployed binary. It does not
reboot the device or alter Zigbee state.

After successful verification, commit the implementation changes with a
descriptive message. When the worktree already contains unrelated changes,
stage and commit only files changed for the current task.

Check the external dashboard rather than using `curl` on the device (it is not
installed there):

```sh
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/status
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/logs
curl --connect-timeout 5 --max-time 8 -k -sS https://192.168.178.62/api/status
```

The dashboard's **Restart service** button uses the following endpoint. It
gracefully restarts only the supervised native daemon; it does not reboot the
device or replace Zigbee state:

```sh
curl --connect-timeout 5 --max-time 8 -sS -X POST http://192.168.178.62/api/service/restart
```

The dashboard serves HTTP on port 80 and HTTPS on port 443. The coordinator
thread is the sole owner of the NCP serial connection and `Gateway` state.
Each HTTP listener has an acceptor plus two request workers; all mutations use
the coordinator command queue. Status reads use protected snapshots; log reads
validate the RAM-backed circular file directly without locking the coordinator.
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

A contact opening is only an alert candidate. Its per-device settings default
to a ten-minute first `Window open` notification and five-minute reminders;
closing it cancels the pending alert.

Secure join preparation waits at most ten seconds for each NCP policy reply.
If one is lost, preparation is cancelled and the dashboard permits another
joining request instead of remaining stuck indefinitely.

Temperature and humidity sensors receive standard report configuration on
discovery (five-minute maximum interval). As a bounded fallback for devices
that ignore it, each known measurement sensor defaults to active-endpoint
refresh every five minutes; its dashboard setting can select another whole
minute interval or `0` to disable fallback polling. The coordinator then
refreshes attributes after the device responds. This is intentionally
serialized for sleepy battery devices.

Coordinator logs are intentionally kept in `/tmp/microhad-zigbee-native.log`, a
bounded 1,000,000-byte binary ring in RAM. Read them through `/api/logs`.
Permanent radio captures belong under the
ignored `diagnostics/` directory and must never be committed.

Warning: daily operation must not write logs, diagnostics, or other
high-frequency state beneath `/tuya` (flash). Keep transient output in `/tmp`
and verify new runtime behavior does not introduce flash write cycles.

## Garage Android app

`garage-app/` is a separate Git repository copied into this workspace and
ignored by the native gateway repository. Commit changes to the Garage app in
`garage-app/` separately from changes to this repository. Do not stage or
commit Garage app files in the native gateway repository.
