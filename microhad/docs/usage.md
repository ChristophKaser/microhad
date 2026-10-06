# Using the native gateway

Installation and backend selection are covered in the [README](../README.md).
The examples below using a development address must be changed to your gateway address.

## Pairing and alerts

Open the HTTPS dashboard and select **Allow joining (5 min)**, then reset or
put the sensor in pairing mode while the displayed join countdown is active.
The gateway accepts standard Zigbee 3.0 joining and sends the standard IAS Zone
Enroll Response required by generic contact/window sensors. Joined devices are
identified by their IEEE address and retain their assigned IAS zone ID across
restarts.

The normalized device model is model-independent: IAS contact state, standard
Temperature Measurement (`0x0402`), Relative Humidity Measurement (`0x0405`),
Occupancy Sensing (`0x0406`), Power Configuration battery percentage, link
quality, and last-seen state are retained and surfaced in the dashboard. An
opened contact starts a ten-minute notification window, followed by five-minute
reminders while it remains open; closing it cancels the pending alert.
Contact sensor settings can switch to Trigger mode, which sends an immediate
push for each known open/closed transition and suppresses further transitions
for the configured cooldown (60 seconds by default). Trigger alerts use the
separate **Triggers** notification channel in the Pommern Garage Android app;
its notification settings can be managed independently in Android settings.
Temperature thresholds can be set per device in the dashboard.

eWeLink TH01 devices have a dedicated reporting profile: temperature/humidity
maximum intervals are one hour, with changes reported sooner when the device
accepts the configuration. The deployed profile still needs live acceptance and
sustained-reporting verification; a quiet 15-minute window is not sufficient to
establish failure or success. See [current TH01 findings](../TH01_HERDSMAN_FIX.md).

Temperature and humidity devices also have a per-device fallback refresh
interval in their settings. It defaults to five minutes for devices that
ignore standard reporting; set it to `0` to disable automatic fallback refresh
while retaining the manual Refresh action.

## External HTTP devices

Use **More ⋯ → Add external device** to add a Shelly or Fritz!Box. Shelly setup
supports a Uni Gen1, Plug Gen1, or Plug S Gen1 by its local host or address. The Uni can expose one selected
digital input as a contact, an optional temperature probe, and one relay as an
actor. Plugs expose their relay as an actor. Optional HTTP Basic credentials
are stored in `shelly-devices.jsonl` in the runtime directory with mode `0600`.
Shelly readings, actors, and contact alerts appear alongside Zigbee devices in
the dashboard's device type groups. A Uni appears as separate contact,
temperature, and relay cards; a Plug combines its switch and power reading.
The status API lists all device sources in `devices` with stable `id` and `source`
fields; Shelly history and actor commands use the regular `/api/devices/{id}`
routes. `/api/shelly/config` manages only the Shelly connection settings.

Fritz!Box setup uses local TR-064 credentials to discover known Wi-Fi hosts.
Selected clients appear as connected/not-connected presence sensors. The
Fritz!Box reports Wi-Fi signal strength as a percentage, available only while
a client is connected. Shelly and Fritz!Box polling share one outbound device
worker. Fritz!Box settings are stored in `fritz-boxes.tsv` with mode `0600`.

Shelly devices poll every 30 seconds by default; the interval can be changed
from 5 to 3600 seconds. Callback mode can replace polling for contact and relay
state. Enable it in the device settings and set the gateway HTTP address that
the Shelly can reach, then copy the generated URLs into the
matching **btn_on_url / btn_off_url** (Uni input) and **out_on_url / out_off_url**
(relay) actions in the Shelly interface. Callback URLs contain a secret token;
keep them private. The Uni thermometer continues polling because Gen1
temperature actions only report threshold crossings, not each reading. Select
**None** for a Uni without a temperature probe.
Plug power is polled even in callback mode. The shared RAM history records
changed power readings in 0.1 W units and does not repeat unchanged readings.

For Browser Push, visit the HTTPS dashboard, accept the certificate, select
**Enable browser alerts**, grant the browser permission, then select **Test
alerts**. The Pommern Garage app registers its FCM token from the home LAN at
`http://192.168.178.62`. The dashboard's **Phones** section lets you label
each installation and enable or disable its alerts. FCM sends directly to
enabled phones using the configured service account. The legacy `firebase_topic`
config field is accepted but no longer used for delivery. The test action uses
the configured delivery transports for an end-to-end check.

## Remote control of lights

In **Scenes & groups**, assign lights to a group. Open **Configure remotes** to
choose a control mode for each discovered remote. **Gateway scenes** keeps the
existing button-to-scene rules. **Native light control** binds the remote's On/Off and Level Control
commands to the selected group, so compatible remotes can operate the lights
without the gateway daemon. Gateway scene rules remain saved but are inactive
for that remote until Gateway scenes is selected again. The dashboard shows
whether native binding is pending, active, or failed; press a sleepy remote's
button again if provisioning is waiting for it to wake. Remotes that reject
group binding are tried with individual bindings to the group's lights.

Native control while the gateway is powered off also requires the remote to
have another reachable Zigbee parent, such as a powered bulb or router.

## Live diagnostics

The dashboard includes a live coordinator log, refreshed every two seconds from
`/api/logs`. The on-device log is a circular binary file at
`/tmp/microhad-zigbee-native.log`; on this gateway `/tmp` resolves below the
RAM-backed `/var` mount, not the `/tuya` flash volume. Its total size is at
most 1,000,000 bytes. The coordinator writes one record at a time, and HTTP
workers read the ring directly without locking or waiting for the coordinator.
Use `/api/logs` to read the log as text. Diagnostics do not create flash write
cycles or consume unbounded memory.

### Event-loop diagnostics

Event-loop diagnostics are off by default and can be toggled while the service
is running. Send a form-encoded POST to `/api/debug/event-loop` with
`enabled=true` to start, or `enabled=false` to stop:

```sh
curl --connect-timeout 5 --max-time 8 -sS -X POST \
  -d 'enabled=true' http://192.168.178.62/api/debug/event-loop
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/logs
curl --connect-timeout 5 --max-time 8 -sS -X POST \
  -d 'enabled=false' http://192.168.178.62/api/debug/event-loop
```

While enabled, the coordinator adds one `EVENT_LOOP_DEBUG` line about every
five seconds to `/api/logs`. It reports loop counts, whether the loop skipped
waiting for commands or immediate work, `select()` results and readiness, and
the most recent poll deadline. For periodic reporting, the wait is capped at
five seconds while diagnostics are enabled. Output remains in the existing
512 KiB RAM-backed log ring; it is never written beneath `/tuya`.
Disable diagnostics after collecting a few intervals to avoid extra wakeups
and profiling overhead during normal operation.

### Sampling a hot MIPS thread

`tools/gateway_cpu_sampler.c` samples the program counter of one running task
without restarting the gateway. Build it with the same MIPS toolchain used for
the gateway:

```sh
export RSDK_ROOT=../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129
"$RSDK_ROOT/bin/mips-linux-gcc" -O2 -static \
  -o /tmp/gateway_cpu_sampler tools/gateway_cpu_sampler.c
```

Copy the executable to the gateway's RAM-backed `/tmp`, identify the hot task
with `top`, and run the sampler as root with that task's PID:

```sh
top
/tmp/gateway_cpu_sampler TID [samples]
```

The sample count defaults to 100 and can be set from 1 to 1000. Each sample
attaches to the selected task, waits for its ptrace stop, reads MIPS EPC from
register slot 34, detaches, and sleeps for 10 ms. The output lists each
sampled instruction address and how many samples landed there. A larger count
at an address suggests the task spent more sampled time there; these counts
are estimates, not precise CPU percentages. Attaching briefly pauses that
task and can interrupt a blocking system call, so use a bounded sample count.
The tool reads registers only and writes no runtime data beneath `/tuya`.

The gateway binary is stripped. To translate addresses into function names,
use a matching unstripped MIPS build with debug information on the development
machine, for example:

```sh
addr2line -f -C -e /path/to/matching-unstripped-binary 0xADDRESS
```

The executable and source revision must match the running gateway binary for
the result to be reliable. Do not deploy the unstripped binary as the service.

Sensor cards offer a History dialog for temperature, humidity, and contact
state. The coordinator writes fixed-size binary records to
`/tmp/microhad-zigbee-native-history.bin`. V2 stores 128 samples per
device/metric page in an initially 2 MiB page pool; it can grow in the
exceptional case where every page is a protected device/metric's only page.
History is returned in pages of up to 2,000 samples. Temperature and humidity
are sampled at most once per metric per minute, while each contact transition
is retained. The RAM-backed history survives a service restart but is cleared
by a device reboot. See [Sensor history format](../SENSOR_HISTORY_FORMAT.md) for
the V2 binary layout and retention rules.

## Web UI development

The dashboard source is in `webui/src`. Build the React and TypeScript UI with
Vite before deploying:

```sh
make webui
```

`deploy.sh` uploads the contents of `webui/dist` to `<runtime_dir>/webui` when
web UI deployment is selected. Deploy the updated binary together with the new
UI the first time, because the server must support bundled assets. The service
worker is built from `webui/public/sw.js` and served as `/sw.js`.

For local development, run `cd webui && npm ci && npm run dev`. Open
`http://localhost:5173` or `http://<this-computer-ip>:5173` on the LAN. Vite
proxies `/api/` requests to the development gateway at `192.168.178.62`, so
no CORS support is required. Browser-push enrollment is disabled in this mode;
test it from the deployed HTTPS dashboard instead.

## Coordinator boundary

The EZSP backend contains the native ASH transport (escaping, CRC, framing and
reset/retry primitives) and a coordinator service boundary. EZSP command
coverage is intentionally isolated in `Gateway` so further Silicon Labs NCP
commands and generic ZDO/ZCL capability handlers can be added without touching
the HTTP, state, or notification contracts.

The enabled Ziggurat backend replaces that radio connection with the local COBS
client. Both keep gateway mutations on the coordinator thread. See
[protocol and state ownership](../ZIGGURAT_INTEGRATION.md).
