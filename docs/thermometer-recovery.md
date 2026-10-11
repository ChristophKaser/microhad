# Diagnose Zigbee sensor reporting

Use this guide when a paired temperature/humidity sensor stops updating.
An inventory entry or successful attribute read does not prove unsolicited
reporting works. Start with [dashboard usage](native-usage.md) and check network
readiness before changing the sensor.

## Check in order

1. Check Zigbee online/formed status and the sensor's last-seen time.
2. Wake a battery device once and use **Refresh**. If reads work but reports do
   not, inspect binding/reporting configuration rather than resetting the network.
3. Request `POST /api/devices/<ieee-address>/reconfigure` if discovery or reporting
   setup is incomplete. Keep the device awake while queued setup finishes.
4. Verify successful Bind and Configure Reporting responses, then an unsolicited
   report. A polling response is separate evidence.
5. Observe through the configured maximum reporting interval, or trigger a
   controlled temperature/humidity change exceeding its reportable threshold.

Successful configuration is persisted in `devices.json` and retained through
ordinary rejoins/restarts. A reset that has no preceding leave announcement can
need explicit reconfiguration. Avoid repeatedly resetting an already configured
sleepy device.

## Reporting policy

Generic temperature/humidity devices use a 30-second minimum, five-minute
maximum, and changes of 0.20 °C / 1% RH. A separate fallback refresh defaults to
five minutes and can be disabled per device with interval `0`.

Only devices identifying as manufacturer `eWeLink`, model `TH01`, receive this
profile on their advertised measurement endpoints:

| Attribute | Type | Minimum | Maximum | Change |
| --- | --- | ---: | ---: | ---: |
| Temperature `0x0402/0x0000` | int16 `0x29` | 30 s | 3600 s | 20 (0.20 °C) |
| Humidity `0x0405/0x0000` | uint16 `0x21` | 30 s | 3600 s | 100 (1% RH) |
| Battery voltage `0x0001/0x0020` | uint8 `0x20` | 3600 s | 65000 s | 0 |
| Battery percentage `0x0001/0x0021` | uint8 `0x20` | 3600 s | 65000 s | 0 |

Setup binds measurement/power clusters first. TH01 receives no extra
measurement reads between configuration writes; battery attributes are read
after configuration. Some TH01 revisions report signed humidity; the parser
accepts that encoding only for this manufacturer/model.

A quiet 15-minute window is inconclusive for a one-hour maximum. Sustained
TH01 reporting still needs physical acceptance; do not infer success from host
tests or one read.

## Host diagnostics

`microhad/scripts/diagnose_ziggurat_reporting.py` provides observation, APS trace
and reporting readback. Set `SENSOR_IEEE` to the sensor’s displayed IEEE address;
`--ieee` overrides the tool’s development-device default. Trace uses the existing private COBS socket and installed
maintenance bridge, without opening another UART owner.

Run from the repository root, using a new private output directory:

```sh
mkdir -m 700 /tmp/thermometer-diagnostic
python3 microhad/scripts/diagnose_ziggurat_reporting.py --host "$GATEWAY_IP" --ieee "$SENSOR_IEEE" \
  --mode observe --duration 900 \
  --output /tmp/thermometer-diagnostic/observation.jsonl
python3 microhad/scripts/diagnose_ziggurat_reporting.py --host "$GATEWAY_IP" --ieee "$SENSOR_IEEE" \
  --password-file "$PASSWORD_FILE" --mode trace --duration 900 \
  --output /tmp/thermometer-diagnostic/aps.jsonl
python3 microhad/scripts/diagnose_ziggurat_reporting.py --host "$GATEWAY_IP" --ieee "$SENSOR_IEEE" \
  --password-file "$PASSWORD_FILE" --mode readback --duration 35 --execute \
  --output /tmp/thermometer-diagnostic/readback.jsonl
```

Readback sends read requests and needs an awake device. Trace rotates bridge
sessions before their 120-second watchdog. Output files are private and never
overwritten. Keep captures out of Git.

## Developer checks

```sh
make -C microhad host test
make -C microhad mips
python3 microhad/scripts/test_diagnose_ziggurat_reporting.py
python3 ziggurat/scripts/test-fresh-join-replay.py
```

The Ziggurat fresh-join patch separates network replay state from a newly joined
child so a reset sensor can communicate. Keep its encrypted synthetic regression
when changing join/replay handling. Host coverage validates profile construction
and replay behavior; hardware report timing remains a separate check.
