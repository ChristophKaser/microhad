# MicroHAD Companion

An MIT-licensed Android companion for one MicroHAD gateway. It provides address
setup, optional LAN discovery, Matter-over-Bluetooth commissioning, FCM push
registration and notifications, and a link to the gateway dashboard. It contains
no household-specific device controls, gateway passwords, Firebase project
credentials, or personal build flavors.

Android 9 or later is required. Push and QR scanning use Google Play services;
manual-code Matter commissioning does not require Firebase or Play services.
The app source is MIT licensed; Android, Firebase and Google Play dependencies
retain their own licenses and service requirements.

## Build

Install a complete JDK 17 and Android SDK platform/build tools for API 35. Set
`JAVA_HOME` and `ANDROID_HOME` to their local paths, then run:

```sh
cd io.github.microhad
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

The APK is `app/build/outputs/apk/debug/app-debug.apk`. Install it with Android
Studio or `adb install -r app/build/outputs/apk/debug/app-debug.apk` on an
explicitly selected development phone. No `google-services.json`, local
password file, Firebase Gradle plugin, or production signing key is needed.
Release signing and store publication are operator-managed and not configured.

See [gateway setup and usage](../docs/companion-app.md) for Firebase project
configuration, discovery, certificates, and acceptance checks.

## Implementation

The gateway returns public Firebase client options from `/api/companion/config`.
The app persists these in private, backup-excluded preferences and initializes
the default Firebase app from them on process startup, including background
message delivery. WorkManager obtains a token and uploads it to the selected
gateway, with exponential retry for transient errors. Token-refresh callbacks
schedule registration again. Each worker carries a configuration generation;
obsolete workers cannot use a replacement gateway profile.

HTTPS API calls and Matter WebSocket relay share an OkHttp transport policy.
Normal certificate validation is the default. An explicit exception pins the
exact SHA-256 certificate fingerprint. No exception is global, and redirects
are disabled. The external browser manages its own certificate trust.

The Matter implementation preserves gateway-owned PASE, Thread provisioning,
fabric identity and CASE; the foreground phone only scans/connects Bluetooth
and relays opaque packets. Setup codes are not persisted or logged. The Matter
screen is protected against screenshots and clears setup input on backgrounding.
The current gateway controller's attestation policy still applies.

Disconnect invalidates registration work, attempts gateway removal and token
deletion, clears local notifications, and closes the process. Reopen the app to
configure a gateway; this ensures a different Firebase project starts with a
fresh default Firebase instance. An unreachable old gateway may retain its
registration; remove it from the dashboard when reachable.

## Verification

Local tests cover Matter codes, advertisements, GATT state, relay bounds,
commissioning lifecycle, gateway HTTP/WebSocket protocol, runtime configuration,
address validation, certificate matching, and retry policy. Native host tests
cover the configuration endpoint and mDNS encoding/parser bounds.

Real phone Bluetooth, LAN discovery, push delivery after process death/reboot,
and accessory commissioning require the manual acceptance checks in the usage
guide. Synthetic tests do not establish those hardware behaviors.
