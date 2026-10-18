# Android companion app

[MicroHAD Companion](../companion-app/README.md) is the general-purpose Android
app for commissioning Matter devices and receiving gateway notifications. It
supports one gateway per installation and opens the existing dashboard for
controls and configuration. It is MIT licensed and builds without household
configuration or a bundled Firebase project.

## Connect a phone

1. Install the companion APK on Android 9 or later and open it.
2. Enter a gateway hostname, IPv4/IPv6 address, or full `http://` / `https://`
   address including a nonstandard port if needed. A bare hostname uses HTTP.
   Alternatively select **Discover on LAN**, choose a result, then **Connect**.
3. The app validates `/api/companion/config`, saves the gateway and initializes
   Firebase when configured. Android 13+ asks for notification permission;
   **Enable notifications / Android settings** opens the settings for changing
   that decision later. Push registration can run independently
   of permission, but Android will not display blocked notifications.
4. **Check gateway / retry push registration** checks reachability and schedules
   registration. Its completion updates the home screen status.
5. **Open gateway dashboard** opens the configured address in your browser.

No password is built into the app. This version uses the gateway's existing
trusted-network API access model; it does not add an account system, remote
access tunnel or authentication proxy support. HTTP remains unencrypted. Use
HTTPS when your network requires encrypted communication.

HTTPS normally requires an Android-trusted certificate. For a self-signed,
expired or differently named certificate, choose **Allow an untrusted HTTPS
certificate…**, verify the displayed SHA-256 fingerprint through a trusted
channel, and explicitly accept it. That exact certificate is pinned for API
and commissioning WebSocket connections. A replacement certificate requires
resetting/reconnecting and accepting its new fingerprint. Browser trust is
separate from the app's exception.

Use **Change gateway / reset** to disconnect. The app invalidates pending
registration work, attempts to remove the gateway registration and FCM token,
clears settings and notifications, then closes. Reopen it for setup. If cleanup
cannot contact the old gateway, remove the old entry from its **Push clients**
page. Reset also applies when changing the gateway's Firebase project/options.

## Configure Firebase on the gateway

Push requires a Firebase project operated by the gateway owner. There is no
shared project or hosted relay supplied by this repository. In that project:

1. Register an Android app with package name `org.microhad.companion`.
2. Obtain its Firebase application ID, API key, project ID and project number
   (messaging sender ID) from the Android client configuration. Keep package
   and signing-certificate API restrictions compatible with your APK.
3. Configure the gateway's existing sending service account and CA bundle.
   Enable the FCM HTTP v1 API and grant the account permission to send messages
   for the same project.
4. Add these public client fields to the gateway configuration:

```json
{
  "gateway_name": "Home gateway",
  "companion_discovery": 1,
  "firebase_application_id": "1:123456789012:android:0123456789abcdef",
  "firebase_api_key": "REPLACE_WITH_ANDROID_CLIENT_API_KEY",
  "firebase_project_id": "your-firebase-project",
  "firebase_sender_id": "123456789012"
}
```

Merge these fields into your existing private configuration; do not replace it
with this fragment. The service account stays in `firebase_service_account`,
and `ca_bundle` continues to select the gateway's outbound TLS CA bundle.
Client configuration is intentionally served to phones; the private service
account is never included. Do not copy a service-account private key into any
client field or the app project.

All four client fields must be present together, and the client project must
match `project_id` in the configured sending service account. Invalid or partial
configuration fails gateway startup before services begin. Omit all four fields
to disable companion push configuration while retaining commissioning and
browser push. Existing Garage registrations continue to use their existing
API; there is no registration format migration.

Configuration changes take effect at the next gateway service start. Follow
[deployment safeguards](deployment.md); this document does not authorize a
deployment or restarting an existing gateway.

Once the phone reports **Registered**, inspect the gateway's Push clients page.
Existing per-phone enable/label/presence controls apply. Use the dashboard's
notification test to check delivery. The global push switch still controls
sending. Keyed alert messages update one notification; `clear` messages remove
it when the alert resolves. Google Play services, internet connectivity and OS
background policies affect delivery; commissioning remains available without
FCM configuration.

## LAN discovery

MicroHAD advertises `_microhad._tcp.local` over IPv4 mDNS on active multicast
interfaces. The SRV record contains its HTTP port, the TXT record contains
`schema=1`, and instance names include the interface IPv4 address encoded in
hexadecimal. The app verifies the selected server via its configuration endpoint
before saving it. Discovery is a convenience, not authenticated pairing.

Discovery uses bounded packets and interface state, three startup probes,
periodic announcements, conflict renaming and TTL-zero withdrawal on graceful
shutdown. Interfaces are refreshed every ten seconds. Discovery failure does
not prevent HTTP serving. Set `companion_discovery` to `0` to disable it.

Phone and gateway must share a multicast-capable LAN. Guest Wi-Fi, client
isolation, VPN routing, blocked UDP 5353, an occupied mDNS port or IPv6-only LANs
can prevent discovery; use manual entry in those cases. Manual IPv6 addresses
are supported, but this version advertises IPv4 addresses only. Discovery stops
on backgrounding or after twelve seconds. Permission/system restrictions are
reported with manual setup remaining available.

## Matter commissioning

Select **Commission Matter device** after connecting. Put the accessory in
pairing mode, enter its manual code or scan its QR code, select the matching BLE
advertisement and keep the app in the foreground until completion. Grant
Bluetooth permissions when requested. Android 9–11 also require location
permission/services for Bluetooth scanning. QR scanning requires Google Play
services; manual entry remains available without them.

If Thread has not been configured, the app offers to create a network using the
gateway's existing policy. See [Matter dashboard](../microhad/docs/matter-dashboard.md)
for importing an existing dataset and observing commissioning progress using
the displayed session ID. The phone does not store the gateway's fabric identity
or Thread dataset. Existing gateway attestation and accessory support limits
are unchanged.

## API contract

`GET /api/companion/config` returns HTTP 200 on both gateway listeners:

```json
{
  "schema": 1,
  "name": "Home gateway",
  "matter": true,
  "firebase": {
    "application_id": "1:123456789012:android:0123456789abcdef",
    "api_key": "ANDROID_CLIENT_API_KEY",
    "project_id": "your-firebase-project",
    "sender_id": "123456789012"
  }
}
```

`firebase` is `null` when client configuration is absent. `matter` indicates
compiled support; the Matter status APIs determine runtime readiness. The
response contains no file paths or service-account fields.

The app obtains an FCM token and submits JSON to
`POST /api/push/devices/register` with `installation_id` (a random UUID retained
until reset), `model`, and `token`. It does not use a separate registration
server. Refreshing a token updates the existing installation. Transient network
errors and HTTP 408/429/5xx retry with exponential backoff; rejected certificates
and other HTTP errors require user correction. No token or setup code is logged.

## Acceptance checks

Automated verification:

```sh
make -C microhad host test
make -C microhad mips
cd companion-app
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

Before claiming phone/device support, manually check:

- Fresh install and manual setup, discovery selection, no discovery result,
  unreachable gateway, invalid client config and no Firebase configuration.
- Trusted HTTPS, explicitly accepted untrusted certificate, and rejection after
  replacing that certificate; exercise both registration and Matter relay.
- Permission grant/denial, Bluetooth off, Google Play services unavailable,
  manual/QR commissioning, cancellation and background cleanup with an accessory.
- Notification show/update/clear in foreground/background, process restart,
  reboot, offline retry, token refresh and notification permission denial.
- Reset with reachable/unreachable old gateway, then configure a different
  Firebase project and confirm old work cannot register with the new gateway.

A force-stopped Android app may require a user launch before push delivery
resumes. No physical phone/accessory or live Firebase acceptance is established
by the local build and synthetic tests.

References: [Firebase runtime initialization](https://firebase.google.com/docs/reference/android/com/google/firebase/FirebaseApp),
[Android NSD](https://developer.android.com/develop/connectivity/wifi/use-nsd),
[Multicast DNS](https://www.rfc-editor.org/rfc/rfc6762.html).
