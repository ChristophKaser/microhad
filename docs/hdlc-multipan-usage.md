# Shared Zigbee and Thread radio transport

Ziggurat is the sole owner of the EFR32 UART. Two-instance HDLC RCP firmware
exposes Zigbee on Spinel IID 1 and Thread on IID 2. Both networks use one channel
and share transmission time. The Matter service runs OpenThread in userspace;
this transport does not provide a Linux border-router interface.

## Build and configure

Follow [build setup](building.md) for firmware and target applications:

```sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
```

Use the multipan build even for Zigbee-only IID 1 operation. The RCP UART runs at
460800 baud with RTS/CTS. The paired launcher selects the COBS API socket
`/tmp/ziggurat/native.sock` and, when Matter is enabled, the HDLC Thread socket
`/tmp/ziggurat/thread.sock`. These carry different protocols and must be distinct.

For a synthetic host RCP, an example server invocation is:

```sh
ziggurat --device tcp://HOST:PORT --spinel-iid 1 \
  --api cobs-unix --listen unix:/tmp/ziggurat/native.sock \
  --thread-socket /tmp/ziggurat/thread.sock
```

Create a private parent directory first. Thread accepts one local consumer. Its
OpenThread URL is `spinel+hdlc+unix:///tmp/ziggurat/thread.sock?no-reset`.
The adapter rejects UART URLs and physical reset requests.

## Routing and bounds

Logical clients share the physical reader/writer and radio lock but have
independent transaction IDs, pending replies and property subscriptions.
Incoming packets are routed by IID before transaction matching. Invalid or
duplicate interface registrations fail.

The Thread endpoint maps consumer IID 0 to radio IID 2, assigns its own upstream
transaction ID, and restores the consumer's IID/TID in responses. IID 2
notifications become IID 0. The radio lock serializes these requests with
Ziggurat's exclusive operations. Thread notifications use a bounded 256-message
queue; overflow can lose traffic and needs sustained-load acceptance.

## Shared-radio policy

- Only the physical owner resets the radio. Secondary resets return
  `LastStatus=InvalidState` without reaching the RCP.
- A physical reset cancels both interfaces' pending requests and notifies
  consumers. Each stack must restore its configuration.
- The first shared channel setting fixes the process channel. Conflicting
  settings are rejected, including after reset. Coordinated channel migration
  is not implemented.
- Promiscuous mode, disabling source matching/radio/idle RX and scans are
  rejected while both interfaces are registered. Use a matching explicit
  Zigbee/Thread channel.

On MG1, ambiguous source-match decisions can consult another PAN's table when
short addresses overlap. Global driver controls are also shared. Hardware
acceptance must cover sleepy children, address collisions, PAN broadcasts,
extended addresses, RF coexistence and recovery. A successful synthetic
handshake does not establish those behaviors.

## Verify changes

Prepare the patched host server using [Ziggurat's README](../ziggurat/README.md), then:

```sh
python3 ziggurat/scripts/test-simulate-rcp.py
python3 ziggurat/scripts/test-hdlc-multipan.py
python3 ziggurat/scripts/test-cobs-unix.py
python3 micro-thread-matter-daemon/scripts/test-openthread-compatibility.py
```

These use synthetic traffic. For userspace UDP details, see
[OpenThread transport](rs-matter-openthread-transport.md); for state ownership,
see [native integration](native-ziggurat-integration.md).
