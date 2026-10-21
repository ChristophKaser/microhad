# Supported devices

Support depends on the features a device exposes, not just its brand or model
name. A device pairing successfully does not mean every feature is supported.
This page describes the implemented features and their compatibility limits;
it is not a list of individually certified products.

## Zigbee

The gateway supports these standard Zigbee functions through either its EZSP
or Ziggurat backend:

| Device category | Supported functions | Limits |
| --- | --- | --- |
| Lights, switches and relays | On/off; brightness for dimmable lights; groups and scenes | Color and color-temperature control are not implemented |
| Temperature and humidity sensors | Readings, history and temperature alerts | Reporting frequency and battery-device wake behavior vary by device |
| Door/window contact sensors | Open/closed state, history and alerts | Requires standard contact reporting; manufacturer-specific formats are not universally supported |
| Motion/occupancy sensors | Occupied/unoccupied state and history | Additional presence features require their own support |
| Remotes and buttons | Recognized button commands can trigger scenes; compatible remotes can bind directly to lights/groups | Button layouts, events and direct binding vary by device |
| Battery-powered devices | Battery readings where exposed, last-seen and link quality | Not every device supplies battery percentage or responds while asleep |

Locks, thermostats, blinds/covers and arbitrary manufacturer-specific functions
have no general application support. A multifunction device may expose a
supported sensor or switch while its other functions remain unavailable.

Battery sensors may need waking during discovery/configuration. If a sensor
pairs but stops updating, use [sensor reporting troubleshooting](sensor-reporting.md).
Do not reset the gateway network to resolve one device's missing readings.

## Matter over Thread

Matter uses a phone-assisted Bluetooth commissioning flow, followed by Thread
for normal operation. The following functions are implemented and covered by
synthetic integration tests. Physical accessory compatibility, sustained reports
and recovery still require [hardware acceptance](matter-physical-acceptance.md).
There is no verified Matter product list yet.

| Device category | Implemented functions | Limits |
| --- | --- | --- |
| Lights, switches and relays | On/off and brightness; confirmed command readback | No color/color-temperature controls |
| Contact sensors | Boolean contact state, history and alerts | Device must expose the supported Boolean State feature |
| Environmental sensors | Temperature, humidity, air quality, CO₂ and PM2.5 readings/history | Only advertised supported measurements are mapped; missing values remain unknown |
| Remotes and buttons | Supported switch events and OnOff/Level bindings | Depends on advertised events and binding capabilities; physical behavior remains unverified |

Matter-over-Wi-Fi devices and general Thread border routing are outside the
implemented gateway path. Other Matter device functions, including locks,
thermostats and covers, are not mapped into application controls. Zigbee and
Thread must share a channel. See [Matter setup](../microhad/docs/matter-dashboard.md)
for commissioning and network requirements.

## Local HTTP devices

| Integration | Supported devices/features | Limits |
| --- | --- | --- |
| Shelly Gen1 Uni | Selected digital input, relay and optional temperature probe | Configure the exposed channels; no probe means no temperature reading |
| Shelly Gen1 Plug / Plug S | Relay control and power readings/history | Power continues polling even when callbacks are enabled |
| Fritz!Box TR-064 | Selected Wi-Fi clients as presence sensors; signal strength while connected | Requires local credentials; presence history is intentionally omitted |

Other Shelly generations/models and arbitrary HTTP devices are not covered by
these integrations. See [external-device setup](native-usage.md#add-shelly-or-fritzbox-devices)
for polling, callbacks and credentials.

## Gateway hardware

The tested gateway is the Silvercrest SGWZ 1 A1 (HG06339) with RTL8196E CPU,
EFR32MG1B232 radio and factory Linux. Other gateway hardware is not validated.
Ziggurat and Matter require the project's replacement radio firmware;
the EZSP backend uses NCP firmware. See [installation](installation.md).
