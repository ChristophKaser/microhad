# MicroHAD Native

`microhad-zigbee-native` is a native, statically linked C++03 gateway daemon for
the Lidl/Tuya gateway. It is built specifically with the bundled RSDK for the
gateway's 32-bit big-endian MIPS/uClibc environment; it does not use low.js,
Node.js, or the TCP serial bridge.

## Build

```sh
make host
make test
make mips
```

`make mips` uses only
`../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129/bin/mips-linux-g++`.
It does not silently fall back to the host compiler.

The MIPS build also compiles the vendored mbedTLS source with the same RSDK.
This supplies the native TLS and cryptographic primitives required by the FCM
and Browser Push transports; it never links the host-built copies from low.js.

## Runtime configuration

Copy `config.example.json` to a protected runtime directory such as
`/tuya/native/config.json`. The runtime directory must not be committed. TLS
certificate/key and Firebase service-account JSON paths are deliberately
outside this repository.

```sh
/tuya/tuya-zigbee-native --config /tuya/native/config.json
```

## Deployment

Build the target first, then upload it with the Dropbear-compatible deployment
script. By default this replaces the target binary and dashboard together, and
retains its configuration and TLS files:

```sh
make mips
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native-test
```

Use `--binary-only`, `--webui-only`, or `--both` to explicitly choose the
assets to upload. Configuration, TLS installation, activation, and service
restart require a binary deployment; a web-UI-only update takes effect without
a daemon restart.

Pass `--config` to atomically replace configuration as well:

```sh
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --config /secure/path/config.json --target /tuya/native
```

The script stages selected files below the target and renames them only after
successful transfer. It does not stop low.js, serialgateway, or any native
daemon; taking ownership of `/dev/ttyS1` is an explicit later step.

To make the native gateway the boot-time service, deploy to `/tuya/native`
with `--activate`. This saves the previous `/tuya/tuya_start.sh` as
`/tuya/tuya_start.sh.lowjs-backup` before installing the supervised native
startup script. Include TLS and CA files when notifications or HTTPS are used:

```sh
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --config /secure/path/config.json --target /tuya/native --activate \
  --tls-certificate /secure/path/cert.pem --tls-private-key /secure/path/key.pem \
  --ca-bundle /etc/ssl/certs/ca-certificates.crt
```

The web API is deliberately LAN-only in v1. With `tls_certificate` and
`tls_private_key` set, `https_port` is served directly over TLS and the
dashboard can register its service worker for Browser Push. Without both
paths the `http_port` listener remains plain HTTP, but Browser
Push subscription is unavailable in browsers because service workers require
a secure context. The defaults are HTTP port 80 and HTTPS port 443. The daemon opens `/dev/ttyS1` directly; do not run
`serialgateway` at the same time.

## Pairing and alerts

Open the HTTPS dashboard and select **Allow joining (5 min)**, then reset or
put the sensor in pairing mode while the displayed join countdown is active.
The gateway accepts standard Zigbee 3.0 joining and sends the standard IAS Zone
Enroll Response required by generic contact/window sensors. Joined devices are
identified by their IEEE address and retain their assigned IAS zone ID across
restarts.

The normalized device model is model-independent: IAS contact state, standard
Temperature Measurement (`0x0402`), Relative Humidity Measurement (`0x0405`),
Occupancy Sensing (`0x0406`), Power Configuration battery percentage, link
quality, and last-seen state are retained and surfaced in the dashboard. An
opened contact emits an alert immediately and repeats while it remains open;
temperature thresholds can be set per device in the dashboard.

Temperature and humidity devices also have a per-device fallback refresh
interval in their settings. It defaults to five minutes for devices that
ignore standard reporting; set it to `0` to disable automatic fallback refresh
while retaining the manual Refresh action.

For Browser Push, visit the HTTPS dashboard, accept the certificate, select
**Enable browser alerts**, grant the browser permission, then select **Test
alerts**. FCM uses the configured topic/service account for the same sensor
events. The test action intentionally uses the configured delivery transports,
so it is the appropriate end-to-end check after credentials are installed.

## Live diagnostics

The dashboard includes a live coordinator log, refreshed once per second from
`/api/logs`. The on-device log is deliberately stored only at
`/tmp/microhad-zigbee-native.log`; on this gateway `/tmp` resolves below the
RAM-backed `/var` mount, not the `/tuya` flash volume. It keeps at most 128
entries of up to 240 characters, so diagnostics do not create flash write
cycles or consume unbounded memory.

## Web UI development

The dashboard source is `webui/index.html`; deployed gateways load it from
`<runtime_dir>/webui/index.html`. To develop the UI locally while using the
development gateway API, run:

```sh
python3 webui/dev_server.py --device http://192.168.178.62
```

Open `http://localhost:8000` or `http://<this-computer-ip>:8000` from another
machine on the LAN. The local server proxies `/api/` requests to the gateway so
no CORS support is required. Browser-push enrollment is deliberately disabled
in this mode; test it from the deployed HTTPS dashboard instead.

## Current protocol boundary

The repository contains the native ASH transport (escaping, CRC, framing and
reset/retry primitives) and a coordinator service boundary. EZSP command
coverage is intentionally isolated in `Gateway` so further Silicon Labs NCP
commands and generic ZDO/ZCL capability handlers can be added without touching
the HTTP, state, or notification contracts.
