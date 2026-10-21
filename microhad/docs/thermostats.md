# Thermostats

MicroHAD provides thermostat cards in the Web UI, device settings, weekly
schedules, temperature history, and an HTTP control API. Features and writable
settings depend on the supported model and its firmware. Temperatures use
Celsius.

## Supported devices

| Model | Connection | Support |
| --- | --- | --- |
| [SONOFF TRVZB](#sonoff-trvzb) | Zigbee | Heating controls, advanced settings, device schedules, battery, and external temperature input |

Currently, thermostat recognition and commands are implemented for the SONOFF
TRVZB only. Other Zigbee thermostats are not automatically supported merely
because they expose the standard thermostat cluster. Matter temperature sensors
can supply external readings, but Matter thermostat control and exposing a
Zigbee thermostat as a Matter thermostat are not implemented. Firmware updates
are not provided.

## Using a thermostat

Pair supported Zigbee thermostats through the gateway's normal join control.
Their cards appear under **Thermostats**, with measured temperature, target
temperature, heating mode, heating status, and battery when reported. Expand
**Thermostat settings** for advanced controls, temperature source selection,
weekly schedules, and diagnostics. Unsupported settings are disabled and
unknown readings are shown explicitly.

Device commands run through the coordinator queue. A successful request means
that work was queued; reported settings change after device confirmation or
readback. The UI shows pending requests and device rejection or timeout errors.
Sleepy battery devices may respond when they next wake. Refresh or reconfigure
an existing paired thermostat without removing its network state.

Weekly schedules execute on the thermostat itself. Saving a day updates that
day without rewriting the rest of the week. Supported modes, schedule limits,
and temperature ranges are listed under each model below.

## External temperature sensors

Choose a measured Zigbee, Matter endpoint, or external-device temperature
sensor under **Thermostat settings → Temperature source**. A sensor can feed
multiple thermostats. Thermostat-derived readings are excluded to prevent feedback
loops. The source's identifier and fallback timeout survive service restarts.

Changed temperatures are coalesced to at most one write per minute per thermostat.
The gateway refreshes unchanged input every five minutes while it is fresh.
It confirms the initial input before selecting external control. The accepted
temperature range depends on the thermostat model; see its section below.

A missing, invalid, deleted or stale source causes a request to use the built-in
sensor. The default freshness timeout is 90 minutes; choose 5–1440 minutes to
suit the sensor's reporting interval. Only an actual temperature observation
refreshes this timer, including reports with an unchanged value. Unrelated
battery, humidity or connectivity traffic does not. After a gateway restart,
the source must report again. A valid new reading automatically resumes external
control. The UI distinguishes requested fallback from confirmed fallback and
shows transmission failures.

Switch to **Built-in sensor** or **Manual / API input** to remove the automatic
link. Manual input writes are rejected while a link is active.

## HTTP API

Send URL-encoded `field` and `value` parameters to
`POST /api/devices/{device-id}/thermostat`, following the gateway's existing
request authentication rules. Requests have bounded retries and surface
rejection/timeouts in `thermostat.error`. Device-specific fields and numeric
encodings are listed in the model sections below.

The gateway manages these external-sensor settings:

| Field | Value |
| --- | --- |
| `temperature_source` | `builtin`, `manual`, or a stable source ID from `temperature_sources` |
| `stale_minutes` | Whole minutes, 5–1440; default 90 |

`/api/status` includes `temperature_sources` with individual endpoint IDs,
names, and temperature observation times. Thermostat devices have
`has_thermostat: true` and a `thermostat` object with:

| Property | Meaning |
| --- | --- |
| `values` | Reported thermostat readings and settings, keyed by field name |
| `supported` | Availability of each thermostat attribute |
| `pending` | Setting names with queued or outstanding requests |
| `schedules` | Seven daily schedules, Sunday through Saturday; null while unknown |
| `source_id` | Linked temperature sensor ID; empty when no automatic link exists |
| `stale_minutes` | External reading freshness timeout |
| `source_observed_at` | Linked sensor's last temperature observation time, in Unix seconds |
| `status`, `error` | Current operation/forwarding status and transmission errors |

## SONOFF TRVZB

MicroHAD recognizes the Zigbee model ID `TRVZB` and handles its standard
thermostat cluster (`0x0201`) and SONOFF settings cluster (`0xfc11`). Its On/Off
cluster is not exposed as a light control.

### Pairing

Mount and calibrate the valve first. Turn its dial to `OF`, then hold its button
for three seconds until the signal icon flashes. Use the gateway's normal join
control to admit it.

### Controls and settings

The card provides off, auto (weekly schedule), and heat modes, plus a 4–35 °C
target in half-degree increments. Settings include child lock, window detection,
frost protection, calibration, valve limits, control accuracy, boost/timer, and
smart temperature control. Valve calibration step counts and motor voltages are
read-only diagnostics. Battery reports can take up to 24 hours.

Valve opening and closing limits require firmware 1.1.4 or newer; control
accuracy requires 1.3.0 or newer. Firmware checks and attribute responses
determine which controls are available. Smart temperature control disables opening-limit
and control-accuracy writes while enabled.

External temperature input accepts 0–99.9 °C in tenths of a degree. Reported
sensor selection 2 means the valve has detected an offline external source and
is using its built-in sensor; selection 3 is normalized to external selection 1
when the source reconnects. Only selections 0 and 1 may be written.

### Weekly schedules

Each day accepts one to six ordered `HH:mm/temperature` transitions, starting at
`00:00`. Temperatures must be 4–35 °C in half-degree increments. For example,
`00:00/18 07:00/21.5 22:00/17` sets three heating periods. Select **Auto** mode
to use the schedule. The gateway answers the valve's time requests using the
TRVZB's Unix UTC epoch.

### Device API fields

| Field | Value |
| --- | --- |
| `occupied_heating_setpoint` | 4–35 °C, step 0.5 |
| `system_mode` | 0 off, 1 auto, 4 heat |
| `local_temperature_calibration` | −12.7–12.7 °C, signed tenths |
| `child_lock`, `open_window` | 0 off, 1 on |
| `frost_protection_temperature`, `timer_mode_target_temp` | 4–35 °C, step 0.5 |
| `valve_opening_degree`, `valve_closing_degree` | 0–100%, firmware ≥1.1.4 |
| `temperature_accuracy` | −1 to −0.2 °C, step 0.2, firmware ≥1.3.0 |
| `smart_temperature_control` | 0 off, 2 on; disables opening-limit/accuracy writes |
| `temporary_mode_select` | 0 boost, 1 timer |
| `temporary_mode_duration` | Whole minutes, 0–180 boost or 0–1440 timer; 0 cancels |
| `external_temperature_input` | 0–99.9 °C, step 0.1, manual mode |
| `temperature_sensor_select` | 0 built-in, 1 external, manual mode |
| `schedule_0` … `schedule_6` | Sunday … Saturday; transition string |

Protocol reference: the upstream
[SONOFF converter](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/devices/sonoff.ts)
and [TRVZB device documentation](https://www.zigbee2mqtt.io/devices/TRVZB.html).
