# Shared Zigbee and Thread radio transport

Ziggurat is the sole owner of the EFR32 UART. Two-instance HDLC RCP firmware
exposes Zigbee on Spinel IID 1 and Thread on IID 2. Both networks use one channel
and share transmission time. The Matter service runs OpenThread in userspace;
this transport does not provide a Linux border-router interface.

## Build and configure

Configure the private [workspace env file](workspace-settings.md) and follow
[build setup](building.md) for firmware and target applications. Before raw host
shell or Docker Compose commands, source `scripts/workspace-env.sh` and call
`workspace_load "$PWD"` from the repository root. The host file is not installed
on the gateway. In gateway commands below, replace `GATEWAY_IPV4` with the
IPv4 value configured as `GATEWAY_IP` on the host.

Build the target server:

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

## TCP endpoint for host OTBR

The multipan server also accepts `--thread-listen IP:PORT` instead of
`--thread-socket PATH`. For example, with all other launch arguments unchanged:

```sh
ziggurat --device /dev/ttyS1 --spinel-iid 1 \
  --api cobs-unix --listen unix:/tmp/ziggurat/native.sock \
  --thread-listen GATEWAY_IPV4:9998
```

On an updated paired installation, switch the running supervisor through its
control helper instead of launching a second UART owner:

```sh
/tuya/native/paired_control.sh thread-tcp GATEWAY_IPV4:9998
# Return Thread to the local service:
/tuya/native/paired_control.sh thread-tcp off
```

The helper validates the IPv4 address/port, writes a request under
`/tmp/ziggurat-supervisor`, and signals Ziggurat as with a log-level change.
The supervisor stops `mthread-matter` before starting Ziggurat with
`--thread-listen`, suppresses Matter respawn while TCP is selected, and keeps
MicroHAD running. Returning to `off` recreates the Unix endpoint and resumes
Matter with its existing state. Diagnostic switches do not consume failure
restart budgets. The supervisor retains diagnostic restart intent across
transient UART/socket release failures, so a retry does not unnecessarily
restart MicroHAD. The selection survives child restarts, but every fresh
supervisor start or reboot defaults to the Unix endpoint and local Matter;
no launcher environment or persistent network configuration is changed.
Inspect `/tmp/ziggurat-supervisor/active-thread-tcp` for the applied selection;
the helper's acknowledgement means the request was queued, not that startup
has completed. Older supervisors without that marker refuse the command.

The underlying endpoint flags are mutually exclusive and require IID 1. With
neither flag, no Thread listener is opened. Bind an explicit gateway address
reachable by the OTBR host; this diagnostic interface has no authentication
or TLS. The control helper currently accepts IPv4 addresses; direct Ziggurat
invocations also accept bracketed IPv6 socket addresses.

TCP carries the same raw HDLC bytes, IID translation and reset/channel policy
as the Unix endpoint. It enables TCP_NODELAY, serves one consumer, rejects
additional connections while it is active, and accepts a new consumer after
it disconnects. Ziggurat continues to own the physical UART at 460800/RTS-CTS.

For the pinned `bnutzer/otbr-tcp` image, configure:

```yaml
environment:
  RCP_HOST: "${GATEWAY_IP:?Set GATEWAY_IP on the host}"
  RCP_PORT: "9998"
  RCP_BAUDRATE: "460800"
  OTBR_RCP_ADDITIONAL_ARGS: "&no-reset"
```

Its [agent startup script](https://github.com/bnutzer/docker-otbr-tcp/blob/main/etc/s6-overlay/s6-rc.d/otbr-agent/run)
appends `OTBR_RCP_ADDITIONAL_ARGS` to the UART URL of the container's internal
TCP-to-PTY transport. This yields a URL ending in
`?uart-baudrate=460800&no-reset`; verify that in the selected image before
connecting. No separate gateway relay or serialgateway process is needed.
The host image still uses its internal PTY adapter. `no-reset` suppresses
initialization resets; Ziggurat independently rejects secondary reset requests.
Do not enable physical reset or reset recovery against the shared radio.
Keep Thread on the existing Zigbee channel and retain an isolated OTBR state
volume; stock OTBR startup compatibility still requires a live smoke test.

Re-read `/tmp/ziggurat-supervisor/active-thread-tcp` after reboot or supervisor
restart. Verify listener ownership, native continuity, Zigbee health and local
Matter restoration for the installed release. A successful Spinel version query
does not establish OTBR attachment or accessory commissioning. Follow the
[host OTBR procedure](matter-otbr-commissioning-diagnostic-plan.md#primary-path--host-otbr-through-ziggurat-tcp-without-flashing)
for those checks.

## Supply the Thread channel

OTBR gets the channel from its Thread Operational Dataset. Neither
`--thread-listen`, `RCP_PORT` nor `OTBR_RCP_ADDITIONAL_ARGS` selects it.
Before switching consumers, record the running Zigbee coordinator's channel
and verify that the saved Thread dataset contains the same channel. Ziggurat
rejects a different channel; `no-reset` does not bypass that check.

Disable OTBR auto-attach until the intended dataset has been imported. With
`INTENDED_DATASET_HEX_FILE` containing only the existing dataset's hex TLVs,
configure the isolated OTBR instance before starting Thread:

```sh
docker compose exec -T otbr wrap-ot-ctl thread stop
docker compose exec -T otbr wrap-ot-ctl ifconfig down
docker compose exec -T otbr wrap-ot-ctl dataset set active "$(cat "$INTENDED_DATASET_HEX_FILE")"
docker compose exec -T otbr wrap-ot-ctl dataset active
docker compose exec -T otbr wrap-ot-ctl ifconfig up
docker compose exec -T otbr wrap-ot-ctl thread start
docker compose exec -T otbr wrap-ot-ctl channel
```

Require successful command responses. Check the active dataset's `Channel`
against the recorded Zigbee channel before enabling the interface, then verify
`channel` reports that same value after startup. `wrap-ot-ctl` is the selected
container image's wrapper for its configured Thread interface; use `ot-ctl`
with the appropriate interface option in images without that wrapper.

Reuse the existing dataset unchanged for this comparison. `dataset channel N`
changes the dataset being prepared and is appropriate when deliberately
creating a separate test network, followed by committing that dataset before
startup. Do not use it to modify the existing network or make a mismatched
import appear compatible. If OTBR initialization tries a conflicting channel
before import is possible, stop and inspect that startup request; do not change
Zigbee's channel or relax Ziggurat's shared-radio policy.

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
python3 ziggurat/scripts/test-hdlc-multipan.py --thread-tcp
python3 ziggurat/scripts/test-cobs-unix.py
python3 micro-thread-matter-daemon/scripts/test-openthread-compatibility.py
```

These use synthetic traffic. For userspace UDP details, see
[OpenThread transport](rs-matter-openthread-transport.md); for state ownership,
see [native integration](native-ziggurat-integration.md).
