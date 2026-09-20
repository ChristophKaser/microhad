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

The web API is deliberately LAN-only in v1. With `tls_certificate` and
`tls_private_key` set, `https_port` is served directly over TLS and the
dashboard can register its service worker for Browser Push. Without both
paths the compatibility `http_port` listener remains plain HTTP, but Browser
Push subscription is unavailable in browsers because service workers require
a secure context. The daemon opens `/dev/ttyS1` directly; do not run
`serialgateway` at the same time.

## Current protocol boundary

The repository contains the native ASH transport (escaping, CRC, framing and
reset/retry primitives) and a coordinator service boundary. EZSP command
coverage is intentionally isolated in `Gateway` so further Silicon Labs NCP
commands and generic ZDO/ZCL capability handlers can be added without touching
the HTTP, state, or notification contracts.
