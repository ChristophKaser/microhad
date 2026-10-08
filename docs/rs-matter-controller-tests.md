# Simulated Matter controller protocol checks

The pinned rs-matter controller now passes a reproducible host-only protocol
harness: PASE, fabric installation, CASE, reads, invokes and stale-session
recovery work against an in-process accessory. Added sensor reads preserve
positive, negative and unknown temperatures. The gateway owner has now explicitly selected skipping manufacturer attestation.
The shared gateway policy maps that choice to rs-matter's existing bypass while
retaining PASE/CASE. A gateway helper now sends Thread credentials and connects
them over PASE; this is exercised against a synthetic network handler. Real
BLE/OpenThread integration and device acceptance remain pending.

## Reproduce

From `ziggurat-compilation` on the development host:

```sh
python3 thread-matter/scripts/test-rs-matter-controller.py
```

The runner pins rs-matter commit
`1570356ecb938a9d19be032b93ca3bb487069e75`, Rust `nightly-2026-10-02`, and the
tracked workspace dependency lock `thread-matter/controller-tests.lock`.
It creates a separate ignored `build/thread-matter/commissioning-source` worktree,
applies only `thread-matter/patches/rs-matter-commissioning-probe.patch`, verifies the exact
tracked diff, rejects unexpected files, and uses `cargo test --locked` with
`--no-default-features --features 'os rustcrypto'`. It runs suites sequentially,
requires a nonzero number of passing tests, and writes a filtered JSON report.
Compiler/test logs stay in ignored `build/thread-matter/`; the tracked shareable
record is [rs-matter-controller-validation.json](rs-matter-controller-validation.json).

The commissioning fixture binds device and controller sockets to IPv6
loopback, uses public upstream development credentials, and supplies a stub
operational resolver. It performs no mDNS multicast discovery, BLE, Thread
radio operations or gateway deployment. The existing MIPS crypto probe and
its separate lock remain unchanged. Host OS networking features used here
are not a proposed networking dependency for factory Linux; the gateway path
still needs OpenThread's internal IPv6/UDP adapter.

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

## Recorded result

All 40 checks passed on the development host:

| Suite | Passed | Test duration |
| --- | ---: | ---: |
| Extended UDP/TCP commissioning and gateway policy/dataset checks | 5 | 1.16 s |
| PASE | 1 | 0.51 s |
| CASE and Sigma2 retransmission | 2 | 0.98 s |
| Attestation components | 32 | Below displayed 0.01 s resolution |

Compilation time is excluded. The commissioning duration covers both fixture
flows, repeated commissioning and added reads, not one pairing operation.
No gateway test, deployment, production configuration or radio state was
changed during this milestone.

## Owner-selected attestation policy

In this revision, `Commissioner::verify_device_attestation` in
`rs-matter/src/onboard.rs` does not request or verify device evidence. It
returns success only when `allow_test_attestation=true`; otherwise it returns
`ErrorCode::Failure`. The test patch confirms default rejection before the
explicit development-fixture bypass. `thread-matter/src/commissioning.rs` now supplies an explicit application
policy: `verify_manufacturer_attestation=false` by default for this gateway,
as requested by its owner. Setting it true selects upstream's fail-closed
behavior; a verifier remains unimplemented. The fixture uses this same policy
rather than its own hard-coded bypass. No running native controller has been
deployed yet.

Available building blocks include Certification Declaration CMS signature and
identity checks, a PAA lookup store, and DER certificate parsers. They are not
wired into commissioning. A future optional verifier would obtain DAC/PAI and
attestation elements, validate the certificate chain against provisioned PAA
roots, verify the signed nonce/session challenge, and validate the declaration
and advertised identity. The CSR response path also needs its DAC-signed
attestation proof checked; the existing nonce-echo check alone is insufficient.

Matter attestation trust roots are distinct from a gateway web certificate.
The agreed Garage transport remains trusted-LAN HTTP/WebSocket with no trusted
gateway certificate requirement. The owner separately authorized bypassing manufacturer attestation, so its
verifier is no longer a prerequisite for this setup. PASE/CASE authentication,
session encryption, nonces and replay protection are retained.

## Thread provisioning adapter

The pinned upstream commissioner still lacks network provisioning, so the
shared `provision_thread` helper fills that step between credential installation
and commissioning completion. It sends `AddOrUpdateThreadNetwork` with active
dataset bytes, checks the network configuration response, and sends
`ConnectNetwork` with the extended PAN ID only on success. A failed response
stops the flow. The simulator now advertises a Thread root endpoint and checks
successful delivery and both failure statuses over real encrypted PASE.

The borrowed dataset validator checks required fields, bounds, duplicate and
truncated TLVs, lengths, UTF-8 network names, active-vs-pending fields, channel
page/mask and equality with the existing Zigbee channel. Dataset bytes contain
credentials and have no Debug/Display implementation; the helper logs nothing
and does not persist them. This is an initial gateway validation subset, not a
replacement for OpenThread's authoritative dataset validation.

Factory-new ALPSTUGA support additionally needs a supported device flow for
regulatory/network commissioning and Thread dataset provisioning over the
Garage BLE delegate, followed by discovery and CASE over OpenThread. The
Thread dataset must use Zigbee's current channel, and Ziggurat retains sole
UART/reset ownership. Credential persistence, subscriptions and real-device
restart recovery are separate acceptance gates.

Native `MATTER=1` remains rejected until the actual controller
transport/IPC integration are implemented and verified. These host test
durations do not predict gateway commissioning latency, full controller RSS,
or combined Zigbee/Thread throughput.
