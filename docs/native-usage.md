# Use the gateway dashboard

Open `http://<gateway-ip>/` after [installation](installation.md). Check that
Zigbee is online and its network is formed before pairing. Use HTTPS when
enrolling Browser Push. [Matter setup](../microhad/docs/matter-dashboard.md)
uses its own Thread and commissioning controls.

## Pair Zigbee devices

Select **Allow joining (5 min)**, then put the device into pairing mode while
the countdown is active. Wake battery-powered devices while discovery and
configuration finish. Verify a real measurement or control response; a device
appearing in the inventory does not establish that it reports correctly.

The dashboard displays supported contact, temperature, humidity, occupancy,
battery and link-quality values. Generic contact sensors use IAS enrollment;
their zone IDs survive restarts. [Sensor reporting](thermometer-recovery.md)
explains refresh, reconfiguration and sleepy-device troubleshooting.

A temperature/humidity sensor's fallback refresh defaults to five minutes.
Set its interval to `0` to disable automatic refresh while keeping manual
Refresh available. eWeLink TH01 reporting allows up to one hour between reports;
short quiet periods alone do not establish a fault.

## Lights, scenes and remotes

Use **Scenes & groups** to group lights and save scenes. In **Configure remotes**,
choose **Gateway scenes** to run saved button rules, or **Native light control**
to bind a compatible remote directly to its group. Scene rules remain saved
when native control is selected. Pending provisioning may need another button
press to wake the remote; the dashboard shows pending, active or failed binding.
Remotes rejecting group binding are tried with individual light bindings.

Direct remote control with the gateway powered off also requires another
reachable Zigbee parent, such as a powered bulb/router. A broadcast confirmation
indicates mesh propagation; it does not confirm every bulb applied a scene.

With Ziggurat, **Gateway settings** controls `broadcast_passive_ack_quorum`
(1–255, default 8). The dashboard distinguishes the saved value from the value
confirmed by the running stack. Saving persists an override in
`gateway-settings.json`; reconnects reapply it. See [MicroHAD](../microhad/README.md)
for the developer/API behavior.

## Alerts

Contact alerts default to a first **Window open** notification after ten
minutes, followed by five-minute reminders. Closing the contact cancels the
pending alert. Per-device **Trigger** mode sends an immediate notification
on a known state transition and applies a cooldown, defaulting to 60 seconds.
Temperature thresholds are also configurable per device.

The global **Push notifications** switch blocks both automatic and test
notifications, discards pending delivery jobs and retains browser subscriptions
for later re-enabling. The **Phones** section labels app installations and
enables/disables their delivery individually.

For Browser Push, open the HTTPS dashboard, accept the configured certificate,
select **Enable browser alerts**, grant permission, then use **Test alerts**.
Android/FCM delivery needs a configured service account and a registered phone.
The app must use your gateway's LAN address. Trigger alerts have a separate
Android notification channel. TLS/notification credentials are optional for
basic local device operation and must stay outside Git.

## Add Shelly or Fritz!Box devices

Use **More ⋯ → Add external device**. Supported Shelly Gen1 devices are Uni,
Plug and Plug S. Uni can expose a digital input, temperature probe and relay;
choose **None** if no probe is connected. Plugs expose switch and power.
Their controls and readings use the same device cards and history as Zigbee.

Shelly polling defaults to 30 seconds, configurable from 5–3600 seconds.
Callback mode can replace contact/relay polling: set a gateway HTTP address the
Shelly can reach and copy the generated URLs into its `btn_on_url`/`btn_off_url`
and `out_on_url`/`out_off_url` actions. These URLs contain secret tokens.
Temperature and plug power still poll in callback mode.

Fritz!Box uses local TR-064 credentials to discover Wi-Fi hosts. Selected hosts
become presence sensors with signal strength while connected. Presence history
is intentionally omitted. Shelly and Fritz!Box share an outbound polling worker;
credentials are stored privately under the runtime directory.

## History

Sensor cards offer a History dialog. The live V3 store is
`/tmp/microhad-history.bin`, bounded below 2 MiB. It survives a service restart;
committed pages in `<runtime_dir>/history.values` restore history after a reboot.
Uncommitted samples can be lost on power loss. Duplicate values are suppressed;
non-discrete readings are sampled at most once per minute, while contact and
other discrete transitions are retained.

Flash persistence keeps at most 255 pages, writes full pages in the background,
and writes dirty sparse pages every four hours. There is no shutdown flush.
Actuator on/off/brightness and Fritz!Box presence are excluded. See the
[history format](../microhad/SENSOR_HISTORY_FORMAT.md) for retention and API cursors.

## Diagnose a problem

The live log reads `/api/logs` from BusyBox syslogd's 512 KiB RAM ring. Logging
does not append files to gateway flash. [Recovery tooling](recovery-tooling.md)
explains log levels, readiness checks and orderly stops.

From the development computer:

```sh
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/status"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/logs"
```

**Restart service** restarts only the supervised native daemon. It does not
reboot the gateway or replace network state. Use the
[deployment procedure](deployment.md) to apply launcher or Matter service changes.
