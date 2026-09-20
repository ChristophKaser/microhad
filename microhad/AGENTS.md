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

The dashboard serves HTTP on port 80 and HTTPS on port 443. It is
single-threaded by design, so accepted client sockets have short read/write
deadlines: retain those deadlines when changing HTTP or TLS handling so an
incomplete browser connection cannot pause radio processing.

Coordinator logs are intentionally kept in `/tmp/microhad-zigbee-native.log`, a
bounded 512-line RAM-only ring. Permanent radio captures belong under the
ignored `diagnostics/` directory and must never be committed.
