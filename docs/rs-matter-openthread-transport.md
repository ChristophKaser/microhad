# Rust/OpenThread userspace UDP adapter

This records the original transport checkpoint. Current source and build
commands live under `micro-thread-matter-daemon/`; the service is `mthread-matter`
and uses [CBOR IPC v2](../microhad/docs/matter-ipc-v2.md). Historical limits and
measurements below describe the earlier probe, rather than the current release.

The optional `openthread` Cargo feature connects rs-matter's `NetworkSend`,
`NetworkReceive` and `NetworkMulticast` traits to the pinned OpenThread userspace
IPv6 stack. Both host and factory MIPS synthetic-radio tests passed on
2026-10-03. This is a transport checkpoint; it does not commission a real
accessory, discover an external mesh peer, or enable native `MATTER=1`.

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
oversized sends fail before enqueueing. The initial 1280-byte payload test
stalled after entering the fragmentation path. Delivery at 1232 bytes now
passes. Larger-message fragmentation has not been validated. Multicast
membership is instance-wide; currently one mDNS socket owns its subscriptions.
The proof checks membership calls and unicast delivery to the mDNS port,
not multicast delivery to an external device. mDNS sends use hop limit 255.

## Factory compatibility

The C ABI archive inherits OpenThread's core/FTD compile definitions so its
public structures agree with the linked SDK. Rust and C++ static archives
are linked together using the existing MIPS-I, big-endian, o32, soft-float
factory target. The linker warns about mixed abicalls/non-abicalls objects;
the actual gateway execution described below passed, rather than relying
only on ELF inspection.

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
  --host 192.168.178.62 --password-file ../ssh_password.txt
```

The device command explicitly runs a temporary probe. It requires production
online/formed status, at least 3000 KiB free RAM and enough flash to retain
1 MiB free after staging. Unique binaries are staged in `/tuya/native-test`;
only small state and the synthetic Unix radio socket reside in `/tmp`.
Limits are 8 MiB virtual memory, 1536 KiB sampled RSS, 1024 KiB minimum sampled
free memory and a 22-second kill watchdog. Sampling is once per second and
cannot establish a peak for a short run. Exact temporary paths are cleaned.
Passwords, datasets and production device state are never printed or copied.

## Results and remaining work

The host test delivered payloads of 0, 4, 256 and 1232 bytes, preserving
contents and peer addresses, in 22 ms. The gateway delivered the same sizes
in **60 ms** for the timed transport section, excluding initialization.
It also rejected a third socket, unsafe radio URLs and a 1233-byte payload.
The stripped MIPS executable was **1,146,488 bytes**, SHA-256
`53669c145a8f70ca879bb171a6bb9fc866f0b55f75c0b91e2832910df0693c32`.

The gateway started with 3072 KiB free RAM. One startup sample reported
520 KiB probe RSS and 1512 KiB system free RAM. That sample is neither peak
nor steady-state memory, and system free RAM includes executable page-cache
and relay effects. This short proof gives no reliable CPU utilization figure
or complete Matter-controller memory budget. Before/after production status
remained `online=true`, `network_formed=true`, `coordinator_backend=ziggurat`.
The physical radio was not accessed and temporary cleanup completed.

Next gates are encrypted Matter exchanges through this transport, native
service/IPC and build-flag wiring, Garage BLE relay, operational discovery and
persistent fabric storage, then actual accessory commissioning/control. The
owner-selected manufacturer-attestation bypass is already in the shared
controller policy; PASE and CASE remain enabled. Gateway HTTPS certificate
trust is not required by the selected trusted-LAN HTTP/WebSocket flow.

The later [encrypted commissioning checkpoint](rs-matter-openthread-commissioning.md)
exercises PASE/CASE, real BTP segmentation with synthetic phone callbacks and
Matter control/sensor operations. It also records the revised test memory
budget and failed guards; the earlier smoke-test measurements above remain
historical rather than a complete controller budget.
