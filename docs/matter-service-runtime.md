# Gateway Matter service runtime

The `mthread-matter` executable in `micro-thread-matter-daemon/` is a single-threaded controller process. It owns
OpenThread, the Matter UDP transport, the private store, and a bounded Unix IPC
listener. It creates/restores a private controller authority and fabric;
Thread create/import/restore and commissioning/relay dispatch are implemented.
Production operational SRP/DNS-SD and remote catalog recovery are wired and
host-tested. The deployed gateway also receives temperature and air-quality
reports from Matter-over-Thread sensors.
The native worker supplies protected coordinator observations and caches status
for HTTP. Host integration exercises the production Manager and report path
with synthetic CASE accessory traffic.

The Manager's telemetry queue holds at most 512 values, suppresses identical
consecutive values per subscription, and evicts the oldest value at capacity.

Build with `bash micro-thread-matter-daemon/scripts/build.sh host service` or
`bash micro-thread-matter-daemon/scripts/build.sh mips service`. The stripped target
artifact is `build/micro-thread-matter-daemon/mthread-matter-mips`. No build deploys or starts it.

The native report-integration test uses the host-only
`micro-thread-matter-daemon/scripts/build-test-service.sh` command. It enables the
`controller-service-test-hooks` feature in the isolated
`build/micro-thread-matter-daemon/service-test-target` directory, leaving the production host
and MIPS service outputs untouched. Both build commands enforce this boundary:
the test artifact must contain its fixture environment marker, while host
production and stripped MIPS artifacts must not. The checks use
`micro-thread-matter-daemon/scripts/check-matter-service-test-hooks.py`. Never deploy this test-hook
executable.

Example invocation, once the coordinator provides a private Thread endpoint:

```sh
mthread-matter --socket /tmp/microhad-matter.sock \
  --state-dir /tuya/native/matter \
  --radio-url 'spinel+hdlc+unix:///tmp/ziggurat/thread.sock?no-reset'
```

The adapter accepts only an absolute Unix HDLC endpoint with `?no-reset`; it
cannot open the UART. Unexpected platform reset requests abort this process.
Thread uses OpenThread's userspace IPv6/UDP stack, requiring neither a kernel
IPv6 interface nor TUN. It supports validated create/import/restore, gated by fresh private native
coordinator observations. It waits in `awaiting_coordinator` before initializing
OpenThread until the optional native worker supplies the observation.

The private state directory must be absolute. The store validates owner-only
directory/files, refuses symlinks, and locks out another store owner. The process
changes its working directory there so the pinned OpenThread POSIX `tmp`
settings subdirectory stays within private state. Entropy comes exclusively
from `/dev/urandom`; failure stops the service. No fixture credentials or open
accessory commissioning window are installed in this controller.

First start commits the complete root/intermediate/controller key and certificate
chain, IPK and allocator as one authority record, then installs the corresponding
upstream fabric. An interruption between those writes restores the same identity.
Reload verifies the chain, matching public/private keys, metadata and exact loaded
fabric materials; corrupt or mismatched records fail closed. Reads do not write.
`--administrator-vendor-id` takes a decimal u16 and defaults to 65521 (`0xfff1`,
the development administrator vendor). This metadata is not a manufacturer
certificate. Set it before first initialization; a different value on restart
is rejected. See the native `docs/matter-identity-store-v1.md` for the schema.

IPC uses [length-prefixed CBOR v2](../micro-thread-matter-daemon/docs/matter-ipc-v2.md).
Only the two-byte little-endian length is outside the CBOR envelope. Opcodes 1
and 2 accept empty CBOR maps and return redacted CBOR status maps; their public
HTTP representations remain JSON. Matter reports initialization/fabric readiness;
Thread reports attachment. Opcodes 3–11 decode schema-1 request bodies;
unknown fields are skipped with warnings containing only the opcode and count.
Structural limits are 16 nested containers, 256 map pairs and 512 array entries.
valid operations without implemented lifecycles return `unsupported`, and invalid
bodies return `malformed`, without echoing input. Opcode-12 requests close the peer.
Limits:
four clients, four accepts per loop, 4096 read/write bytes per client per loop,
eight queued responses per client, 2048 bytes per complete CBOR envelope, and a two-second idle
deadline that partial requests cannot extend. OpenThread polls at most five
milliseconds between Matter and IPC turns.

The socket and permanent adjacent `.lock` file are owner-only. A held flock
rejects duplicate starts. An existing private socket is probed nonblocking and
removed only on connection refusal; regular files, symlinks, public sockets,
and live listeners are refused. Normal shutdown removes only the socket inode
created by this invocation. Retaining `.lock` prevents an inode replacement race.
SIGINT/SIGTERM unwind the transport and release storage and IPC ownership.

The production service logs through syslog, including safe unknown-field warnings.
Normal diagnostics exclude request payloads, datasets and store contents.
Trace-level packet diagnostics can expose decrypted data; use them only for
bounded diagnosis. See [logging controls](recovery-tooling.md#check-readiness-and-logs). A missing radio,
transport failure, or corrupt state exits the service. The native project's
optional paired supervisor implements independent bounded retries and backoff;
see [Matter integration](../microhad/MATTER_INTEGRATION.md).

## Verification

Use the [component build and test commands](../micro-thread-matter-daemon/README.md)
and [CBOR contract](../micro-thread-matter-daemon/docs/matter-ipc-v2.md). Synthetic fixtures exercise
CASE/report delivery, persistent native calls, BLE ownership and supervisor
recovery. They complement the deployed temperature and air-quality sensor path;
new accessory categories still need compatibility testing.
