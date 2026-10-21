# SONOFF TRVZB

MicroHAD recognizes the Zigbee model `TRVZB` as a thermostat. It supports the
standard thermostat cluster and SONOFF's FC11 settings, device schedules,
battery reports, and automatic external-temperature forwarding. It does not
provide firmware updates or expose the valve as a Matter thermostat.

Pair using the gateway's normal join control. Mount and calibrate the valve
first; turn its dial to `OF`, then hold its button for three seconds until the
signal icon flashes. Existing paired devices can be refreshed/reconfigured
without removing their network state.

The thermostat card provides off, auto (weekly schedule), and heat modes, plus
a 4–35 °C target in half-degree increments. Settings include lock, window
detection, frost protection, calibration, valve limits, control accuracy,
boost/timer, and smart control. Firmware-gated and rejected attributes are
disabled. Valve step counts and voltages are read-only diagnostics. Battery
reports can take up to 24 hours; unknown readings are not shown as zero.

Weekly schedules are stored and executed on the valve. Each day has one to six
ordered `HH:mm/temperature` transitions, starting at `00:00`; temperatures use
4–35 °C in half-degree increments. Saving one day does not rewrite other days.
The gateway answers the valve's time requests using the TRVZB's Unix UTC epoch.

## External sensors

Choose a measured Zigbee, Matter endpoint, or external-device temperature
sensor under **Thermostat settings → Temperature source**. A sensor can feed
multiple valves. Thermostat-derived readings are excluded to prevent feedback
loops. The source's identifier and fallback timeout survive service restarts.

Changed temperatures are coalesced to at most one write per minute per valve.
The gateway refreshes unchanged input every five minutes while it is fresh.
It confirms the initial input before selecting external control. Temperatures
outside 0–99.9 °C cannot be forwarded.

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
request authentication rules. Successful responses mean the request was
queued; device state changes only after a valid report or readback. Requests
have bounded retries and surface rejection/timeouts in `thermostat.error`.

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
| `temperature_source` | `builtin`, `manual`, or a stable source ID from `temperature_sources` |
| `external_temperature_input` | 0–99.9 °C, step 0.1, manual mode |
| `temperature_sensor_select` | 0 built-in, 1 external, manual mode |
| `stale_minutes` | Whole minutes, 5–1440 |
| `schedule_0` … `schedule_6` | Sunday … Saturday; transition string |

`/api/status` adds `temperature_sources` with individual endpoint IDs, names,
and observation times. Thermostat devices have `has_thermostat: true` and a
`thermostat` object containing reported `values`, `supported` capabilities,
`pending` setting names, seven `schedules` (null while unknown), `source_id`, `stale_minutes`,
`source_observed_at`, `status`, and `error`. Reported sensor selection 2 indicates
the valve has detected an offline external source; only 0/1 may be written.

Protocol reference: the upstream
[SONOFF converter](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/devices/sonoff.ts)
and [TRVZB device documentation](https://www.zigbee2mqtt.io/devices/TRVZB.html).
