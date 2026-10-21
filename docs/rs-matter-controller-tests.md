# Simulated Matter controller protocol checks

The pinned rs-matter controller has a host-only protocol
harness: PASE, fabric installation, CASE, reads, invokes and stale-session
recovery work against an in-process accessory. Added sensor reads preserve
positive, negative and unknown temperatures. The gateway policy skips manufacturer attestation.
The shared gateway policy maps that choice to rs-matter's existing bypass while
retaining PASE/CASE. A gateway helper sends Thread credentials and connects
them over PASE; this is exercised against a synthetic network handler. The
gateway also runs Matter-over-Thread temperature and air-quality sensors; this
fixture remains focused on protocol regression coverage.

## Reproduce

From the repository root on the development host:

```sh
python3 micro-thread-matter-daemon/scripts/test-rs-matter-controller.py
```

The runner pins rs-matter commit
`1570356ecb938a9d19be032b93ca3bb487069e75`, Rust `nightly-2026-10-02`, and the
tracked workspace dependency lock `micro-thread-matter-daemon/controller-tests.lock`.
It creates a separate ignored `build/micro-thread-matter-daemon/commissioning-source` worktree,
applies only `micro-thread-matter-daemon/patches/rs-matter-commissioning-probe.patch`, verifies the exact
tracked diff, rejects unexpected files, and uses `cargo test --locked` with
`--no-default-features --features 'os rustcrypto'`. It runs suites sequentially,
requires a nonzero number of passing tests, and writes a filtered JSON report.
Compiler/test logs stay in ignored `build/micro-thread-matter-daemon/`; the tracked shareable
record is [rs-matter-controller-validation.json](rs-matter-controller-validation.json).

The commissioning fixture binds device and controller sockets to IPv6
loopback, uses public upstream development credentials, and supplies a stub
operational resolver. It performs no mDNS multicast discovery, BLE, Thread
radio operations or gateway deployment.  Host OS networking features used here
are not a proposed networking dependency for factory Linux; the gateway path
uses OpenThread's internal IPv6/UDP adapter.

## Coverage

The extended UDP and TCP commissioning tests exercise:

1. Establish PASE and arm the fail-safe.
2. Reject commissioning with the secure default attestation policy.
3. Explicitly opt into the upstream test-only attestation bypass for the
   public fixture, create root/intermediate/controller operational credentials,
   request a device CSR and install its operational certificate/fabric.
4. Use the stub resolver to establish CASE and send `CommissioningComplete`.
5. Read OnOff false, invoke Toggle, read OnOff true, and read VendorName.
6. Read TemperatureMeasurement over CASE: `2315` (23.15 °C), `-525`
   (−5.25 °C), and Matter null. The fixture changes its local sample and data
   version between reads; this is not a subscription or a physical sensor test.
7. Clear accessory sessions to simulate a reboot and commission again with
   the same controller. The TCP case also tests reuse of a node ID with a stale
   CASE session; the upstream UDP variant avoids that branch because trailing
   MRP acknowledgments can independently clear the stale session.

Separate upstream suites check a PASE handshake, CASE including Sigma2
retransmission, and certification-declaration/trust-store components. Passing
those components does not demonstrate integrated attestation verification.

## Owner-selected attestation policy

In this revision, `Commissioner::verify_device_attestation` in
`rs-matter/src/onboard.rs` does not request or verify device evidence. It
returns success only when `allow_test_attestation=true`; otherwise it returns
`ErrorCode::Failure`. The test patch confirms default rejection before the
explicit development-fixture bypass. `micro-thread-matter-daemon/src/commissioning.rs` supplies an explicit application
policy: `verify_manufacturer_attestation=false` by default for this gateway,
as requested by its owner. Setting it true selects upstream's fail-closed
behavior; a verifier remains unimplemented. The fixture uses this same policy
rather than its own hard-coded bypass.

Available building blocks include Certification Declaration CMS signature and
identity checks, a PAA lookup store, and DER certificate parsers. They are not
wired into commissioning. A future optional verifier would obtain DAC/PAI and
attestation elements, validate the certificate chain against provisioned PAA
roots, verify the signed nonce/session challenge, and validate the declaration
and advertised identity. The CSR response path also needs its DAC-signed
attestation proof checked; the existing nonce-echo check alone is insufficient.

Matter attestation trust roots are distinct from a gateway web certificate.
Garage uses trusted-LAN HTTP/WebSocket. The configured attestation bypass is
independent of gateway web TLS. PASE/CASE authentication,
session encryption, nonces and replay protection are retained.

## Thread provisioning adapter

The pinned upstream commissioner still lacks network provisioning, so the
shared `provision_thread` helper fills that step between credential installation
and commissioning completion. It sends `AddOrUpdateThreadNetwork` with active
dataset bytes, checks the network configuration response, and sends
`ConnectNetwork` with the extended PAN ID only on success. A failed response
stops the flow. The simulator advertises a Thread root endpoint and checks
successful delivery and both failure statuses over real encrypted PASE.

The borrowed dataset validator checks required fields, bounds, duplicate and
truncated TLVs, lengths, UTF-8 network names, active-vs-pending fields, channel
page/mask and equality with the existing Zigbee channel. Dataset bytes contain
credentials and have no Debug/Display implementation; the helper logs nothing
and does not persist them. This is an initial gateway validation subset, not a
replacement for OpenThread's authoritative dataset validation.

Physical accessory acceptance must exercise the phone BLE delegate, Thread
provisioning, operational discovery, CASE, subscriptions and restart recovery.
Ziggurat retains sole UART/reset ownership; Thread uses the observed Zigbee
channel. Host test timings do not predict target latency, peak RSS or sustained
Zigbee/Thread throughput.
