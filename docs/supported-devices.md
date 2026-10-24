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
| Remotes and buttons | Recognized button commands trigger scenes and Lua `remote.action` events; compatible remotes can bind directly to lights/groups | Button layouts, events and direct binding vary by device |
| Thermostats | Standard Thermostat server cluster `0x0201`: reported temperature, heating/cooling targets, operating modes, supported settings and weekly schedules; dashboard, HTTP and Lua controls | Available controls follow device capabilities and reported limits; SONOFF TRVZB additionally supports model-specific settings and external temperature input |
| Battery-powered devices | Battery readings where exposed, last-seen and link quality | Not every device supplies battery percentage or responds while asleep |

Locks, blinds/covers, dedicated fan controls and arbitrary manufacturer-specific
functions have no general application support. A multifunction device may expose
a supported sensor or switch while its other functions remain unavailable.

See [thermostat support](../microhad/docs/thermostats.md) for standard controls,
SONOFF TRVZB settings, schedules and external-temperature limitations. Zigbee
thermostats are not bridged to other Matter controllers.

Battery sensors may need waking during discovery/configuration. If a sensor
pairs but stops updating, use [sensor reporting troubleshooting](sensor-reporting.md).
Do not reset the gateway network to resolve one device's missing readings.

## Matter over Thread

Matter uses a phone-assisted Bluetooth commissioning flow, followed by Thread
for normal operation. The following functions are implemented and covered by
synthetic integration tests. The deployed gateway receives temperature and
air-quality reports from Matter-over-Thread sensors. Other accessory categories
depend on their advertised clusters and the mappings below.

| Device category | Implemented functions | Limits |
| --- | --- | --- |
| Lights, switches and relays | On/off and brightness; confirmed command readback | No color/color-temperature controls |
| Contact sensors | Boolean contact state, history and alerts | Device must expose the supported Boolean State feature |
| Environmental sensors | Temperature, humidity, air quality, CO₂ and PM2.5 readings/history | Temperature and air-quality sensors operate on the deployed stack; only advertised supported measurements are mapped and missing values remain unknown |
| Remotes and buttons | Generic Switch server events produce Lua `remote.action` events; OnOff/Level client clusters support bindings | Command-style client switches do not produce Lua button events unless they also expose the Generic Switch server cluster (`59`) |
| Thermostats | Thermostat server cluster `513`: reported temperature, heating/cooling targets, operating modes, supported settings and weekly schedules; dashboard, HTTP and Lua controls | Controls follow discovered features, attributes, commands and reported limits; commands target individual endpoints and await confirmation/readback; no SONOFF-specific external-temperature forwarding |

Matter-over-Wi-Fi devices and general Thread border routing are outside the
implemented gateway path. Other Matter device functions, including locks,
covers and dedicated fan controls, are not mapped into application controls.
Zigbee and Thread must share a channel. See [Matter setup](../microhad/docs/matter-dashboard.md)
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

## Lua automation support

Supported device readings are available through `devices.get()`, `devices.list()`
and device events. This includes contact, occupancy/presence, water-leak,
environmental, power, battery and signal readings where the integration exposes
them. Actions include light on/off and brightness, actor on/off, thermostat
settings and schedules, and scene activation or group off. `scenes.get()` reads
the gateway's selected scene and group-off state; it does not confirm that every
light matches the scene.

Matter `devices.refresh()` currently selects from a limited set of clusters.
It does not correctly target thermostat or air-quality/CO₂/PM2.5 readings;
those devices' normal reports still update Lua snapshots. This limitation
concerns explicit Lua refresh requests, not the dashboard's thermostat controls.

See [automation examples](scripting.md#example-scripts) and the
[Lua scripting API](lua_scripting_api.md), including
[Matter switch models](lua_scripting_api.md#matter-switch-models).

## Gateway hardware

The tested gateway is the Silvercrest SGWZ 1 A1 (HG06339) with RTL8196E CPU,
EFR32MG1B232 radio and factory Linux. Other gateway hardware is not validated.
Ziggurat and Matter require the project's replacement radio firmware;
the EZSP backend uses NCP firmware. See [installation](installation.md).
