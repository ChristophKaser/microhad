# Gateway Matter service runtime

The `thread-matter` executable is a single-threaded controller process. It owns
OpenThread, the Matter UDP transport, the private store, and a bounded Unix IPC
listener. It creates/restores a private controller authority and fabric;
commissioning commands, Thread lifecycle, DNS-SD, and device reports remain pending.

Build with `bash scripts/build-rs-matter-transport.sh host service` or
`bash scripts/build-rs-matter-transport.sh mips service`. The stripped target
artifact is `build/rs-matter/thread-matter-mips`. No build deploys or starts it.

Example invocation, once the coordinator provides a private Thread endpoint:

```sh
thread-matter --socket /tmp/microhad-matter.sock \
  --state-dir /tuya/native/matter \
  --radio-url 'spinel+hdlc+unix:///tmp/ziggurat/thread.sock?no-reset'
```

The adapter accepts only an absolute Unix HDLC endpoint with `?no-reset`; it
cannot open the UART. Unexpected platform reset requests abort this process.
Thread uses OpenThread's userspace IPv6/UDP stack, requiring neither a kernel
IPv6 interface nor TUN. It does not yet create/import or attach a Thread network.

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

IPC uses the shared version-1 bounded frame format. Opcodes 1 and 2 accept empty
payloads and return redacted JSON. Matter reports initialization/fabric readiness;
Thread reports attachment. Opcodes 3–11 now decode strict schema-1 request bodies;
valid operations without implemented lifecycles return `unsupported`, and invalid
bodies return `malformed`, without echoing input. Opcode-12 requests close the peer.
Limits:
four clients, four accepts per loop, 4096 read/write bytes per client per loop,
eight queued responses per client, 2048 bytes per payload, and a two-second idle
deadline that partial requests cannot extend. OpenThread polls at most five
milliseconds between Matter and IPC turns.

The socket and permanent adjacent `.lock` file are owner-only. A held flock
rejects duplicate starts. An existing private socket is probed nonblocking and
removed only on connection refusal; regular files, symlinks, public sockets,
and live listeners are refused. Normal shutdown removes only the socket inode
created by this invocation. Retaining `.lock` prevents an inode replacement race.
SIGINT/SIGTERM unwind the transport and release storage and IPC ownership.

Stderr contains only static start/stop/failure messages. Request payloads,
CLI inputs, datasets and store contents have no logging path. A missing radio,
transport failure, or corrupt state exits the service. The native project's
optional paired supervisor implements independent bounded retries and backoff;
see `microhad-zigbee-native/MATTER_INTEGRATION.md`. It is not activated in production.

Validation: 31 Rust library tests pass, including six listener checks, three
strict-body groups, five session lifetime checks and four authority recovery checks.
`python3 scripts/test-matter-service.py`
starts the real host service against the synthetic RCP and verifies both JSON
statuses, typed-body rejection, duplicate exclusion, 20 timely calls, socket permissions,
clean SIGTERM, fabric restoration in a second process, unchanged credential file
on reload/status, corruption/vendor refusal, and no RCP reset. Socket tests need local permissions;
the workspace sandbox otherwise returns EPERM. These checks prove host behavior,
not physical mesh attachment or commissioning a real accessory.

The factory uClibc lacks `accept4`. The MIPS adapter supplies an `accept`/`fcntl`
compatibility wrapper; its descriptor flag update is valid for this single-threaded
process, which does not concurrently fork. Existing factory sleep/name shims remain.
With authority creation/validation the package is 1,527,452 stripped bytes,
static big-endian MIPS-I/o32. Strict request decoding was 1,343,120 bytes and
the initial status-only checkpoint 1,228,412 bytes. These are package sizes, not RSS or
physical-device runtime measurements. ELF validation is recorded in the native plan.

The encrypted commissioning fixture now uses the same `Identity` authority,
signer and allocator as this service. Both host BTP and UDP runs pass PASE,
Thread provisioning, CASE and fresh CASE after committed credentials/authority
reopen, with the simulated phone detached. The allocator then returns a value
above the commissioned node. Timings were 217/264 ms on the development host;
the updated factory fixture builds but has not been executed on the gateway.
This does not yet wire the commissioner into the service's IPC job dispatch.
