# TH01 / SNZB-02 herdsman comparison — 2026-10-03

## Current state and next steps

The thermometer is **not recovered**. Hardware failure is not established.
Herdsman's device definition and our native setup differ, and inspection found
a plausible Ziggurat replay-counter failure across repeated fresh joins.
Prioritize software compatibility and a counter-reset regression before another
pairing attempt. No firmware, production configuration, network identity, or
service executable was changed during this source investigation.

1. Reproduce repeated authorized fresh joins on the host with decreasing device
   NWK frame counters. Confirm exactly when replay state must be replaced; do not
   disable replay protection or reset counters on arbitrary received traffic.
2. Match the current upstream TH01 profile in a device-specific configuration
   plan, including battery voltage reporting and operation order. Keep other
   devices' settings unchanged. Validate success responses and persist verified
   configuration status, distinguishing actual joins from ordinary announcements.
3. In a controlled live trace, distinguish MAC polls, network decryption/replay
   rejection, APS ACK/delivery, parent changes, and ZCL responses. Retain parent
   information from COBS DeviceJoined rather than discarding it. Confirm stored
   reporting settings while awake, then observe at least one full reporting hour
   without resetting/pairing again. Send the requested gateway REST alert only
   if user action is needed. No user action is requested by this source review.

These are investigation/implementation steps, not deployed changes. Current
production native remains MD5 `cfea133ccc3f06d088facd95e5df783a`; Ziggurat remains
`53046d62702c6b06f06bc1ef7bf01f0b`. Latest read-only check at 1791030647 still
showed last_seen 1791024454, 22.29 °C, 72.55% humidity and 100% battery. The live
child table currently contains Küche. Mere child presence does not prove its
frames are accepted, nor does earlier child absence prove a bug if it changed
parent legitimately.

## Identification and exact reporting setup

The converter maps Zigbee model `TH01` to SONOFF `SNZB-02` (eWeLink/RHK08 white
label). Endpoint 1 has exactly the recorded input clusters 0, 3, 0x0402, 0x0405,
1 and output cluster 3. Both the vendored definition and current upstream bind
temperature, humidity and battery clusters to the coordinator endpoint.

| Attribute | Current upstream TH01 | Vendored converter | Deployed native |
|---|---|---|---|
| Temperature 0x0402/0x0000 | int16; min 30 s; max 3600 s; change 20 = 0.20 °C | min 5 s; max 1800 s; change 20 | int16; min 30 s; max 300 s; change 20 |
| Humidity 0x0405/0x0000 | uint16; min 30 s; max 3600 s; change 100 = 1% | min 10 s; max 3600 s; change 100 | uint16; min 30 s; max 300 s; change 100 |
| Battery voltage 0x0001/0x0020 | uint8; min 3600 s; max 65000 s; change 0 | min 3600 s; max 7200 s; change 0 | Read only; no reporting configuration |
| Battery percentage 0x0001/0x0021 | uint8; min 3600 s; max 65000 s; change 0 | min 3600 s; max 7200 s; change 0 | uint8; min 3600 s; max 65000 s; change 1 |

Upstream order: bind all three clusters; configure temperature; configure
humidity; configure voltage and read voltage; configure percentage and read
percentage. It does not insert temperature/humidity reads between those
configuration writes. It sets powerSource to Battery in its host database.
There is no TH01-specific manufacturer unlock, keep-awake command, periodic
read loop, or Poll Control binding in this definition. This device does not
advertise genPollCtrl (0x0020); generic Poll Control fast-poll logic must not be
mistaken for a TH01-specific requirement.

The native accepted all three bindings and reporting responses during the
successful wake, so missing voltage reporting and different intervals are real
compatibility differences, **not proven causes** of its later silence. Current
herdsman defaults permit an unchanged value to stay quiet for up to an hour.
Our earlier 15-minute test used requested 300-second maxima, but a later reset
could change those settings; without a fresh readback, it cannot establish a
hardware fault or what settings survived that later join.

The converter processes both attribute reports and read responses. Temperature
has a device-specific validity filter (-100 to +100 °C exclusive); humidity is
scaled by 100 and constrained to 0..100%. No signed humidity configuration
exception is applied: standard uint16 reporting is used. Our actual trace
already confirmed this distinction: TH01 returned int16 humidity for reads,
accepted uint16 Configure Reporting, and sent uint16 humidity reports.

## Configuration lifetime and sleepy-device handling

Zigbee2MQTT keeps verified/configured metadata and avoids repeating configuration
when its definition is already recorded as configured. An ordinary announcement
updates last-seen and invokes implicit check-in; it does not itself invalidate
configured metadata. A **DeviceJoined** event does invalidate configuration,
so a true fresh join is deliberately configured again. End devices are not
configured merely because Zigbee2MQTT starts; setup is triggered when they are
active. Concurrent setup is prevented and automatic attempts are bounded.

Our registerNode path handles COBS Joined and ZDO Device_annce through the same
routine, and starts a full interview again after its 30-second deduplication
window. It has no equivalent durable reporting-configuration marker. This is a
behavioral difference, not proof that configuration writes cause the failure.

Herdsman endpoint methods wait for matching Configure Reporting replies, reject
non-success ZCL status, and record successful reporting configuration in the
host database. Bind also rejects non-success status. Native interview handling
currently logs rejected bind/configuration status and advances. Also, a pending
full configuration behind a refresh remains marked refreshOnly until the refresh
completes: wakeInterview suppresses refresh-only wakeups even when
configurationPending is true. That deserves a focused test before changing wake
semantics for all routine refreshes.

Important qualification: the vendored herdsman has generic sendWhen=active or
fastpoll request queues, flushed by implicit check-in on received traffic and
announcements. **TH01 does not automatically select that mode**: newly created
vendored devices default to immediate; automatic fastpoll/active selection is
only for genPollCtrl devices. Current herdsman likewise queues based on its
pending-request timeout and send policy, not an unconditional TH01 workaround.
Do not claim this queue alone explains our failure. The old EZSP adapter retries
one timed-out ZCL response; the endpoint timeout default is 10 seconds.

There is also an upstream exception: the TH01 converter catches configuration
errors and logs them without rethrowing, because some variants report without
configuration. Consequently a host configured marker is not necessarily proof
of device acceptance for this definition. Our diagnosis must continue using
actual Bind/Configure Reporting statuses and unsolicited reports, not a generic
"interview completed" or metadata flag alone.

The observed report frame control was 0x18, with Disable Default Response set.
Herdsman does not send a ZCL Default Response in that case. Missing a TH01-specific
ZCL default-response workaround is therefore not indicated by those frames.
MAC acknowledgments, frame-pending/poll handling, NWK replay checks and APS
acknowledgments remain separate stack-level requirements.

## Concrete replay-counter hypothesis

Vendored herdsman's EZSP driver handles trustCenterJoinHandler; on an unsecured
fresh join it removes the old trust-center link-key table entry before reporting
the join. Silicon Labs' NCP owns network security, replay protection and child
polling. The JavaScript device converter has no implementation of NWK replay
counter resets, so this comparison does **not** prove precisely which internal
counter-reset policy the closed stack follows.

In our checked-out Ziggurat, joining.rs begin_join calls only
core.aib.aps_security.begin_join(ieee), resetting stale APS link-key state. Its
NWK security object separately remembers the last successfully authenticated
incoming frame counter per **relaying IEEE**. inbound_network_key rejects any
counter less than or equal to that baseline. Searching the whole worktree found
no method/path to clear that NWK baseline when an already-known device joins
fresh; only initialization and insertion are present.

A factory reset may restart the thermometer's network counter. A MAC association
and network-key transport can then complete (and emit Joined) while subsequent
low-counter encrypted reports and application responses are rejected. This is
a concrete source-supported **hypothesis**, matching the successful wake followed
by another Joined and a failed interview. We have not captured a live replay
rejection tied to Küche, and have not yet reproduced the full repeated-join flow
in the host simulator. Do not claim the cause established, globally clear replay
state, or change coordinator outgoing counters/security state to test it.
A child that reattaches via a router must be analyzed using its actual relayer,
not assumed to have the same direct-child replay baseline.

## Sources and reproducibility

Local checkouts reviewed:

- zigbee-herdsman-converters version 15.0.38-hotfix.0, commit 580c1cf5:
  devices/sonoff.js, lib/reporting.js, lib/constants.js,
  converters/fromZigbee.js.
- zigbee-herdsman version 0.14.93, commit 2d1c453:
  src/controller/model/device.ts and endpoint.ts,
  src/controller/controller.ts, src/adapter/ezsp/adapter/ezspAdapter.ts,
  src/adapter/ezsp/driver/driver.ts.
- zigbee2mqtt lib/extension/configure.ts (the working gateway checkout).
- Native src/gateway.cpp configuration planning, registerNode, wakeInterview,
  Joined decoding, Bind and Configure Reporting response handling.
- Patched Ziggurat worktree crates/ziggurat-driver/src/zigbee_stack/joining.rs,
  crates/ziggurat-zigbee/src/nwk/security.rs and aps/security.rs,
  crates/ziggurat-protocol/src/wire.rs DeviceJoinedPayload.

Current upstream was checked by browser, then GitHub commits/master resolved
and exact raw files fetched to verify the SONOFF profile, constants and endpoint
policy against stable revisions. Public source snapshots were kept under /tmp;
private live status evidence remains in the existing diagnostics directory.
No credentials, keys, runtime configuration or raw production snapshots enter
these documentation commits. Search/read tools were rg, sed, git revision reads,
Python urllib for public sources and read-only /api/status, plus the existing
network_backup.ssh_args and ziggurat_cobs.Client for ScanChildren (0x22).

Pinned upstream primary sources:

- [zigbee-herdsman-converters/src/devices/sonoff.ts](https://github.com/Koenkk/zigbee-herdsman-converters/blob/eb77ec10e6d931d277908ade895c93e955a06ccf/src/devices/sonoff.ts).
- [zigbee-herdsman-converters/src/lib/reporting.ts](https://github.com/Koenkk/zigbee-herdsman-converters/blob/eb77ec10e6d931d277908ade895c93e955a06ccf/src/lib/reporting.ts).
- [zigbee-herdsman-converters/src/lib/constants.ts](https://github.com/Koenkk/zigbee-herdsman-converters/blob/eb77ec10e6d931d277908ade895c93e955a06ccf/src/lib/constants.ts).
- [zigbee-herdsman/src/controller/model/endpoint.ts](https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131c705a7cabc07fd3c403f2ddf7e42/src/controller/model/endpoint.ts).
- [zigbee2mqtt/lib/extension/configure.ts](https://github.com/Koenkk/zigbee2mqtt/blob/344b6e03b43644c97154a72ca9eae17193c553a2/lib/extension/configure.ts).

[Official SNZB-02 troubleshooting](https://www.zigbee2mqtt.io/devices/SNZB-02.html)
explicitly documents a stuck/no-data condition and a remove/restart/re-pair
recovery. That is evidence of a known software/network state problem, not a
proof of hardware failure. The destructive force-remove portion was not run.
[Issue 5562](https://github.com/Koenkk/zigbee2mqtt/issues/5562) is the converter's
own justification for tolerating configuration failures; it describes a device
working despite Bind timeouts, not this device's established diagnosis.
