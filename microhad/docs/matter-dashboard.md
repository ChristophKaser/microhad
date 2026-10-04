# Matter and Thread dashboard controls

The Controls tab contains a Matter panel in Matter-enabled builds. Disabled
status hides the panel and all Matter mutations. Missing/stalled service or
coordinator readiness shows an unavailable state and a check/retry action;
controls require validated controller/fabric and Thread readiness snapshots.
The panel uses same-origin HTTP. It requires no installed gateway certificate
and does not alter browser TLS validation or the Zigbee joining controls.

## Set up the Thread network

When no dataset is configured, explicitly create a network or enter/import the
existing operational dataset as lowercase hexadecimal (1–254 bytes). The input
is masked, excluded from autocomplete, cleared before submission, on page
backgrounding and when a configured network is observed. Nothing is stored in
browser preferences/localStorage or diagnostics. The gateway independently
validates TLVs and the fresh observed Zigbee channel; the browser cannot supply
or override the channel. Existing configured networks expose no create/import
controls. The service refuses replacement independently of this UI gate.

## Commission a device through Garage

Open Matter in Garage, enter the local HTTP gateway address shown in the panel,
bring the accessory into commissioning mode, then enter/scan its setup code and
select the matching BLE advertisement. Keep Garage open through completion.
Garage remains the foreground GATT relay; the gateway owns BTP, PASE, Thread
provisioning, fabric credentials and CASE. The dashboard receives no setup code
or PIN and does not take over the phone relay.

Garage commit `7540b17` displays a selectable session ID. Copy it into the
panel's session field and select **Watch progress**. The dashboard polls the
same session's status, displays fixed commissioning phases, remaining gateway
budget and a retained terminal node ID, and provides explicit cancellation.
The session ID correlates traffic; it is not administrator authentication.
A delivery job becoming complete does not mark the accessory commissioned.
Only the returned session state `complete` does that.

Status polling stops at a terminal result, failure or panel unmount. Cancellation
stops current session polling before submitting its separate mutation. Leaving
the dashboard stops its requests but does not cancel a running gateway operation
or the phone relay. Returning requires explicitly selecting a session again.
Retries never automatically repeat create, import or cancellation. Lost replies
can leave an operation running; inspect status or wait for its gateway deadline
before retrying. Queue/service errors are fixed human messages and never include
untrusted response bodies, datasets or BLE traffic.

## Verification — 2026-10-04

`make webui` passed TypeScript checking and the production Vite build. It needed
sandbox escalation for the npm cache/dependency installation. The generated
bundle is about 401 kB (129 kB gzip), including the existing dashboard/chart UI;
build output is not committed.

`python3 scripts/test_matter_webui.py` passed nine tests. It compiles the actual
production Matter client and uses real localhost HTTP fixtures for disabled and
unconfigured status, schema/input bounds, job polling, delivery-versus-session
completion, Thread import, cancellation, capacity/service failures, wrong job
IDs, oversized/error-body redaction and aborted polling without mutation replay.
Requests have a three-second bound, queued operations a ten-second overall
bound, and response retention a 2048-byte cap. Polling is sequential per session,
at two-second intervals after the prior status call finishes.

Garage's two flavor suites still pass 60 tests and both APKs build after session
ID display. These checks establish host/client behavior and compile the UI;
they do not establish rendered browser interaction, Android Bluetooth/camera or
real accessory behavior. No device deployment or plan 08 acceptance occurred.

Device catalog/controls/measurements still depend on plan 07 and remain open in
plan 04. See [API contract](matter-native-api-v1.md),
[plan 04](matter-plans/04-native-api-dashboard.md) and
[plan 05](matter-plans/05-garage-ble-relay.md).
