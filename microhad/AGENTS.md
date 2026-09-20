# Native MicroHAD Gateway

This is a native C++03 gateway for the Tuya MIPS device. Build the target
binary only with `rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129`:

```sh
make host test
make mips
```

`make mips` builds the vendored Mbed TLS dependency and produces the static,
big-endian MIPS binary at `build/mips/microhad-zigbee-native`.

Deploy with `deploy.sh`, passing a local config and password file explicitly.
The permanent device service is `/tuya/tuya_start.sh`; its executable and
configuration live in `/tuya/native`. Never commit device configuration,
credentials, TLS keys, persistent network state, or Zigbee keys.

The dashboard serves HTTP on port 80 and HTTPS on port 443. The HTTP listener
is an incremental nonblocking state machine (up to 16 active clients; no more
than four are accepted per event-loop iteration). Keep it that way: the NCP
serial owner is deliberately single-threaded and must run between client reads
and writes. HTTPS has a short handshake deadline, so an incomplete TLS client
cannot pause radio processing.

Coordinator logs are intentionally kept in `/tmp/microhad-zigbee-native.log`, a
bounded 512-line RAM-only ring. Permanent radio captures belong under the
ignored `diagnostics/` directory and must never be committed.
