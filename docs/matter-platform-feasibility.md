# Matter controller platform gate on the factory MIPS gateway

The optional native build contract is added, but the first Matter prerequisite
gate is **blocked by the factory C++ standard library**. No Matter controller
binary or real-device test was produced. This does not invalidate the earlier
OpenThread proof build or establish an unavoidable hardware limitation.

Pinned SDK: `project-chip/connectedhomeip` `v1.6.1.0`, commit
`3bcdd56ba54fb88b2afb4bfef575014671df7aa7`. The fetch script verifies that commit
and refuses a modified checkout. SDK source and generated probes/reports stay
under ignored `build/matter-sdk/`; the shareable
[compiler validation record](matter-platform-validation.json) is tracked.

```sh
bash scripts/fetch-matter-sdk.sh
python3 scripts/check-matter-platform.py host
python3 scripts/check-matter-platform.py mips
```

Both probes compile an actual upstream
[TypeTraits.h](https://github.com/project-chip/connectedhomeip/blob/v1.6.1.0/src/lib/support/TypeTraits.h)
consumer and representative standard-library requirements used by the
[controller](https://github.com/project-chip/connectedhomeip/blob/v1.6.1.0/src/controller/SetUpCodePairer.h)
and support code. Host programs link and run successfully. Target checks use
factory headers with stock RSDK GCC and Clang 18; they do not silently select
host C++ headers or execute MIPS programs on the host.

| Candidate | Actual SDK type-traits probe | Standard-library probe |
| --- | --- | --- |
| Host GCC, C++17 | Compile/link/run passed | Compile/link/run passed |
| Stock RSDK GCC | Rejects `-std=c++17` | Rejects `-std=c++17` |
| Clang 18 with stock RSDK headers | Missing `std::underlying_type_t` | Missing `<optional>` |

The SDK controller explicitly includes and uses `std::optional`; changing only
the compiler's language flag cannot supply that header. `<atomic>` is also
absent from the factory standard library. A modern compatible MIPS C++ library
and ABI runtime must be ported before full controller linking, crypto,
discovery, storage, and runtime/memory checks. The build must keep MIPS-I/o32,
big-endian, soft-float, and factory uClibc compatibility. Host libraries and
an ELF MIPS header alone are not sufficient evidence of gateway compatibility.

`check-matter-platform.py` is a necessary-prerequisite checker, not a complete
SDK build or capability claim. Exit status is nonzero when no compiler candidate
passes its prerequisite probes. A future successful target compiler/link check
still requires actual target execution and the other platform gates.

The native Makefile now accepts `MATTER=0|1`, defaults off, requires Ziggurat
when selected, defines `MICROHAD_MATTER_ENABLED`, separates outputs, and rejects
enabled builds until these prerequisites and actual integration exist. The
full remaining implementation contract, including the trusted-LAN Garage BLE
relay and factory-new ALPSTUGA acceptance, is in
[native MATTER_INTEGRATION.md](https://github.com/ChristophKaser/microhad-zigbee-native/blob/main/MATTER_INTEGRATION.md).

No production service, UART, radio firmware, dataset, certificate, or Garage
app was changed during this gate. All checks were performed on the development
host. Existing OpenThread CPU/RSS observations apply to that earlier synthetic
OpenThread workload, not to the unbuilt Matter controller.

## Alternative: a smaller independent Matter controller

The failed gate applies to the pinned C++ SDK with the factory C++ library.
It does not require every implementation of the Matter protocol to use that
library. A C/C++03 controller could avoid the dependency, while retaining the
OpenThread internal UDP transport and Garage BLE relay architecture.

The custom controller would still need Matter TLV/message encoding, reliable
exchanges and replay protection, BLE transport framing, PASE/SPAKE2+, device
attestation, Thread/fabric commissioning, certificates and CASE sessions,
discovery, and Interaction Model reads/subscriptions/invokes. ALPSTUGA can limit
the initial application-cluster scope but does not remove commissioning or
secure-session requirements. Existing Mbed TLS supplies useful primitives;
its gateway profile currently disables AES-CCM and PKCS5, so those requirements
need their own feature-gated configuration and verification.

Before implementing those protocols from scratch, test
[upstream rs-matter](https://github.com/project-chip/rs-matter). Its current
README documents `no_std`/no-alloc operation, pluggable IP/BLE transports,
and support for controllers and commissioners. The existing Ziggurat Rust
MIPS build path makes a target experiment plausible; this is an engineering
inference, not demonstrated Matter target compatibility.

The next useful experiment is a pinned Rust controller compile/link with the
custom MIPS target, followed by crypto/secure-channel checks on the gateway,
OpenThread UDP adaptation, and the commissioning BLE delegate. Verify atomics,
runtime helpers, target instruction compatibility, executable size, RAM, and
actual controller APIs. No rs-matter source has been fetched or compiled and no
device acceptance has been performed for this alternative. Upstream memory
figures are not gateway controller measurements.
