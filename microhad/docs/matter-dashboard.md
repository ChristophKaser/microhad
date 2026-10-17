# Use Matter and Thread from the dashboard

Matter-enabled builds show Thread setup and commissioning controls. Disabled
builds hide them. Unavailable service/coordinator state blocks mutations and
shows a check/retry action. Basic panel requests use same-origin HTTP;
Browser Push has separate HTTPS requirements.

## Set up Thread

When no dataset is configured, explicitly create a network or import an existing
operational dataset as lowercase hexadecimal, 1–254 bytes. The gateway derives
the channel from a fresh Zigbee coordinator observation and independently
validates the dataset. The Thread channel must match Zigbee; the browser cannot
override it.

The dataset input is masked and cleared on submission/backgrounding. It is not
saved in browser preferences or diagnostics. Existing networks cannot be
replaced through these controls; identical import is idempotent. If a reply is
lost, inspect status before retrying. Mutations are not replayed automatically.

## Commission through the phone

1. Open Matter in Garage and enter your gateway's local HTTP address.
2. Put the accessory in commissioning mode, enter/scan its setup code and
   select its BLE advertisement. Keep Garage in the foreground.
3. Copy the displayed session ID into the dashboard's session field and select
   **Watch progress** if you want to monitor from the browser.
4. Wait for session state `complete`. A delivery job marked complete only means
   a request finished; it does not mean the accessory was commissioned.

The phone owns GATT and relays opaque BTP packets. The gateway owns PASE,
Thread provisioning, fabric credentials and CASE. The session ID correlates
requests and is not administrator authentication. The dashboard does not take
over the phone relay.

## Scan a Matter QR code in the dashboard

Chrome and Edge can scan a Matter QR code directly in the Device setup panel.
Open the dashboard over HTTPS, select **Scan Matter QR code**, and allow camera
access. The scanner prefers the rear camera, decodes the code locally, then
fills the setup-code field. Select **Commission with this browser** to choose
the accessory and start commissioning; scanning never starts commissioning on
its own. Stop scanning or leave the panel to release the camera. If scanning is
unavailable, denied, or cannot recognize the code, enter the setup code manually
or use Garage.

The browser displays phases, remaining gateway budget and terminal node ID,
and provides explicit cancellation. Leaving the panel stops polling without
cancelling the phone's operation. Return by selecting the session again.
Lost replies may leave an operation running until its gateway deadline.

## Devices, readings and controls

Matter nodes and their cached endpoint descriptors arrive in `/api/status` alongside Zigbee
and Shelly. Supported clusters include OnOff, Level, Boolean State/contact,
temperature, humidity, air quality, CO₂ and PM2.5. Availability and controls follow
advertised capabilities. Unknown/null values remain unknown rather than showing
fabricated measurements. Sensor units follow the service's typed replies.

OnOff/Level commands complete only after a separate CASE readback. Level is
0–254 and transition time is in deciseconds. Cached descriptors/readings may
remain while a node is offline; cached display does not guarantee a command
will succeed. [The HTTP API](matter-native-api-v1.md) describes cached inventory, jobs
and relay ownership.

## Developer verification and limits

```sh
make -C microhad webui
python3 microhad/scripts/test_matter_webui.py
node microhad/webui/test/matter-devices-browser.cjs
```

Run from the repository root after installing Web UI dependencies. These use
synthetic service/browser fixtures. Physical phone/accessory commissioning,
reports and restart recovery remain in [hardware acceptance](matter-plans/08-real-device-acceptance.md).
The current controller policy bypasses manufacturer attestation; enabling
verification fails closed until a verifier is implemented. PASE/CASE remain
active. See [controller policy](../../docs/rs-matter-controller-tests.md).
