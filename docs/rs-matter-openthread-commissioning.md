# Commissioning through OpenThread and a phone BTP relay

`micro-thread-matter-daemon/src/bin/ot_commissioning.rs` exercises the pinned
rs-matter commissioner against a synthetic accessory. PASE/provisioning can use
UDP or gateway BTP with simulated phone GATT callbacks; CASE uses OpenThread.
The fixture covers OnOff/Level controls, signed and nullable sensor readings,
subscriptions, descriptor discovery and persisted authority/fabric reopen.

The production owner allocates a unique node before provisioning, uses its
persisted authority and validated Thread dataset, and completes only after CASE.
The owner-selected manufacturer-attestation bypass leaves PASE/CASE active;
enabling attestation verification remains fail-closed until a verifier exists.
Generated keys use OS entropy. Credentials and packet contents are not logged.

BTP callbacks require the active session/generation. ATT MTUs 23–517 are accepted;
the stack negotiates at most 247, carrying at most 244 bytes per segment. Only
one C1 write is outstanding; completion must match its ID. A failed or stalled
write retires the attempt. Reconnect drops the commissioning future before
replacing the transport. See [session lifecycle](matter-session-lifecycle.md).

## Reproduce on the host

```sh
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh host
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py --commissioning --ble --service-owner --service-discovery
python3 microhad/scripts/test_matter_native_report_integration.py
```

Rebuild with the component script so fixtures use the intended probe artifact.
The service-owner mode uses the production controller/future and operational
discovery. The report integration also checks the separate service-to-native
handoff using an isolated test-hook build; production artifacts exclude it.

The radio and accessory remain synthetic. Physical BLE/GATT interoperability,
Thread RF coexistence and accessory/service/gateway restart acceptance require
explicitly requested device testing. The private native/service transport is
[CBOR IPC v2](../microhad/docs/matter-ipc-v2.md).
