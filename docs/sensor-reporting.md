# Troubleshoot sensor reporting

Use this guide when a paired Zigbee sensor stops updating. See
[supported devices](supported-devices.md) for available features and limits.
An inventory entry or successful manual read does not prove automatic reports
are working.

1. Check that Zigbee is online and its network is formed, then check the
   sensor's last-seen time.
2. Wake a battery-powered device and use **Refresh**. Sleeping devices may
   not respond immediately.
3. If reads work but automatic updates do not, request reconfiguration using
   `POST /api/devices/<ieee-address>/reconfigure`. This repeats discovery,
   binding and reporting setup. Keep the device awake while setup finishes.
4. Check logs for successful configuration and an automatic report. Polling
   replies and unsolicited reports are different evidence.
5. Allow for the device's reporting interval or make a controlled measurement
   change. Quiet periods alone do not establish a fault.

Temperature/humidity sensors have a fallback refresh interval in their settings,
defaulting to five minutes. Set it to `0` to disable automatic refresh while
keeping manual Refresh available. Devices may ignore reporting requests or
use their own reporting intervals.

Completed configuration is retained through ordinary rejoins and service
restarts. A factory-reset device may need explicit reconfiguration. Avoid
repeatedly resetting a device that is already configured, and preserve the
existing network while diagnosing it.

For logs and service readiness, see [recovery and diagnostics](recovery-tooling.md).
