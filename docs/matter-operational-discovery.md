# Operational discovery through userspace OpenThread

The production service runs a resolver alongside Matter transport, OpenThread
polling and IPC. It consumes rs-matter's operational resolve requests, queries
`<compressed-fabric-hex16>-<node-hex16>._matter._tcp.default.service.arpa.` with
OpenThread's DNS client, and deposits validated SRV/TXT/AAAA answers through the
SDK's backend-neutral resolver interface. The SDK expects `.local` instance
labels internally; the adapter translates that suffix without changing the
fabric/node label or injecting an address. No Linux IPv6 socket, TUN interface,
libc hostname resolver, trusted gateway certificate or new UART owner is used.

## SRP and server selection

The OpenThread build enables SRP server/client, DNS-SD server, DNS client, ECDSA
and Thread Network Data services. The service enables an SRP server in normal
unicast mode; the Network Data publisher decides whether its advertisement is
needed. It does not force-add a competing server. Accessories can register
signed services with that server. The local SRP client's auto-start selector
chooses an advertised server and updates the DNS client's default IPv6 address;
the production client registers no host or accessory service. DNS uses UDP/53
and the standard `default.service.arpa.` SRP domain. An imported network can use
its already advertised server; arbitrary DNS domain overrides are not exposed.

SRP records are volatile registry state, with OpenThread lease handling. The
update handler bounds retained hosts/key leases at 64, total services at 128,
services per host at eight, addresses per updated host at eight and TXT data at
256 bytes. A replacement counts its updated host once. Exhausted bounds reject
registration with a protocol error. Ordinary lookup/cache operations write no
TMKV state. OpenThread's existing private settings retain its protocol/security
state; the complete private state directory remains the backup unit.

## Query, validation and lifetime bounds

The C adapter permits one outstanding query. It asks for SRV/TXT plus a follow-up
host address query when AAAA is absent, using one-second response timeouts and
at most two transmissions per stage over userspace UDP. It verifies the exact
requested operational label and service domain, nonzero port and TTL, complete
TXT and at least one address before returning a fixed 332-byte result. At most
four address candidates and 256 TXT bytes are copied. OpenThread parses and
correlates the DNS packets; no response body appears in diagnostics.

Callbacks carry only a monotonically increasing, nonwrapping generation cookie
and write into a static bounded adapter slot. They never point into a Rust
future. Dropping a future discards its result; a pending query retains its slot
until its bounded callback completes. An old token cannot consume another
query's result. Instance deinitialization clears the slot after the SDK stops.

The Rust layer rejects unspecified, loopback, multicast and link-local-only
addresses, removes duplicates, and retains routable IPv6 candidates. Known
SII/SAI/SAT/T TXT fields require bounded numeric ASCII; duplicate keys are rejected
case-insensitively. Unknown extensions, including binary values, are ignored.
There are at most 16 cached operational records. TTL is the minimum received
SRV/TXT/address TTL capped at 120 seconds; reads never renew it. New answers for
the same identity replace old addresses, and expired entries are discarded.
Lookup-time Thread-unavailable observations clear the cache.

An in-flight request can wait for Thread attachment within the SDK's five-second
resolve budget. Query futures are bounded at 4.2 seconds; retry delays are
100/250/500/1000 ms and stop when the SDK request has ended or Thread is lost.
There is no tight polling, unbounded query queue or persistent address cache.
Proactive invalidation after a CASE failure and production reconnection of
catalogued nodes remain plan 07 device/recovery integration work. Very slow
attachment/registration still fails the bounded attempt and needs explicit retry.

## Synthetic encrypted acceptance of this path

The separate probe fixture archive registers a synthetic accessory through the
real SRP client/server protocol, including signature verification and registry
update. It does not deposit an address into the Matter resolver. The fixture
waits for registration, drops a submitted DNS future before pumping its callback,
checks a missing service and then resolves valid SRV/TXT/AAAA. The production
owned commissioner uses the same resolver for PASE/Thread provisioning/CASE and
CommissioningComplete. After persisted authority/fabric reopen, the fixture
uses a fresh resolver and CASE session for OnOff readback with the phone detached.

The registration helper archive is linked only by `controller-probe`. Function
sections and linker garbage collection remove its instance accessor from the
production service. Binary symbol inspection verifies all discovery fixture
symbols are absent. The helper is not reachable through IPC or production Rust.

Commands (host checks require permission for synthetic sockets):

```sh
bash micro-thread-matter-daemon/scripts/build.sh host service
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/micro-thread-matter-daemon/target" cargo +nightly-2026-10-02 test \
  --locked --release --manifest-path micro-thread-matter-daemon/Cargo.toml \
  --features controller-service --lib
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py \
  --commissioning --ble --service-owner --service-discovery
python3 micro-thread-matter-daemon/scripts/test-matter-service.py
bash micro-thread-matter-daemon/scripts/build.sh mips service
bash micro-thread-matter-daemon/scripts/build.sh mips commissioning
```

Initial checks exposed the required Thread Network Data service build option,
pinned SDK public name/service adapter types, premature synthetic GATT before
attach, and a fixture dataset reapply after registration that detached Thread.
The fixture now configures its dataset before SRP startup and forwards GATT only
after the production owner publishes an attach generation. An older MIPS CMake
cache also needed the optional fixture target guarded against a missing source
path; the persistent generator now supplies that guard. Diagnostic tracing was
removed after those failures were resolved.

This is actual userspace SRP/DNS and encrypted host evidence with a synthetic
radio/accessory. It is not physical RF interoperability, target performance,
complete remote catalog recovery or production descriptor/report integration.
Default older fixture modes retain explicit rendezvous for comparison; only the
`--service-discovery` mode proves this path. No deployment or plan 08 action occurs.

Primary API references: [OpenThread SRP](https://openthread.io/reference/group/api-srp),
[DNS service resolution](https://openthread.io/reference/group/api-dns) and
[Thread discovery architecture](https://openthread.io/codelabs/openthread-border-router).
