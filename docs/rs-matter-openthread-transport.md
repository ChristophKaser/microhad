# Rust/OpenThread userspace UDP adapter

The optional `openthread` feature connects rs-matter transport traits to
OpenThread userspace IPv6/UDP. The service is `mthread-matter`, built from
`micro-thread-matter-daemon/`.

## Ownership and limits

`micro-thread-matter-daemon/openthread-transport/adapter.cpp` owns one OpenThread instance and two internal
UDP sockets, reserved in the probe for Matter 5540 and mDNS 5353. Initialization
accepts only absolute `spinel+hdlc+unix:///...?...` URLs with exactly `?no-reset`.
It rejects UART URLs; Ziggurat remains the physical UART/reset owner. The
platform reset callback aborts rather than resetting the shared radio.

OpenThread supplies IPv6 inside the process. There is no Linux IPv6 socket,
TUN interface, or privileged network-interface setup. The configured dataset
must pass the shared validator, including matching the Zigbee channel.
Production integration must supply a private persistent working directory
before OpenThread initialization; the proof uses isolated temporary state.

The Rust owner cannot cross threads. Socket lifetimes keep its instance alive,
and stable heap storage holds receive callbacks until socket closure. Each
socket queues at most eight datagrams. Overflow drops packets; Matter MRP
must handle packet loss. A too-small receive buffer returns an error rather
than silently truncating a packet. This adapter supports UDP/IPv6 only.

The pinned OpenThread build defaults to a 1280-byte IPv6 datagram limit.
Payloads are capped at 1232 bytes to leave 40 IPv6 and 8 UDP header bytes;
oversized sends fail before enqueueing.  Larger-message fragmentation has not been validated. Multicast
membership is instance-wide; currently one mDNS socket owns its subscriptions.
The proof checks membership calls and unicast delivery to the mDNS port,
not multicast delivery to an external device. mDNS sends use hop limit 255.

## Factory compatibility

The C ABI archive inherits OpenThread's core/FTD compile definitions so its
public structures agree with the linked SDK. Rust and C++ static archives
are linked together using the existing MIPS-I, big-endian, o32, soft-float
factory target. Verify target artifacts with the configured toolchain before deployment.

Factory uClibc 0.9.30 lacks two symbols required by Rust std timers. MIPS-only
compatibility functions provide relative `CLOCK_MONOTONIC`/`CLOCK_REALTIME`
sleeps through `nanosleep` and return POSIX error numbers. Absolute deadlines
return `ENOTSUP`; invalid inputs return `EINVAL`. Thread naming returns
`ENOSYS`. Host builds retain their libc implementations. No cryptography or
entropy implementation is replaced. Embassy timers use the generic queue
instead of requiring an Embassy executor-specific waker layout.

## Reproduce

The component build scripts prepare the pinned sources and custom target.
For the standalone transport fixture, run:

```sh
bash micro-thread-matter-daemon/scripts/build.sh host transport
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh host
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py
bash micro-thread-matter-daemon/scripts/build.sh mips transport
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh mips
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-device.py \
  --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE"
```

The device command explicitly runs a temporary probe. It requires production
online/formed status, at least 3000 KiB free RAM and enough flash to retain
1 MiB free after staging. Unique binaries are staged in `/tuya/native-test`;
only small state and the synthetic Unix radio socket reside in `/tmp`.
Limits are 8 MiB virtual memory, 1536 KiB sampled RSS, 1024 KiB minimum sampled
free memory and a 22-second kill watchdog. Sampling is once per second and
cannot establish a peak for a short run. Exact temporary paths are cleaned.
Passwords, datasets and production device state are never printed or copied.
