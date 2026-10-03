# Gateway Matter service runtime

The `thread-matter` executable is a single-threaded controller process. It owns
OpenThread, the Matter UDP transport, the private store, and a bounded Unix IPC
listener. This is the runtime foundation; commissioning commands, persistent
controller identity creation, DNS-SD, and device reports are still pending.

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

Validation: 22 Rust library tests pass, including six listener lifecycle,
fragmentation, overload and timeout checks and three strict-body decoder groups.
`python3 scripts/test-matter-service.py`
starts the real host service against the synthetic RCP and verifies both JSON
statuses, typed-body rejection, duplicate exclusion, 20 timely calls, socket permissions,
clean SIGTERM, and no RCP reset. Socket tests need local socket permissions;
the workspace sandbox otherwise returns EPERM. These checks prove host behavior,
not physical mesh attachment or commissioning a real accessory.

The factory uClibc lacks `accept4`. The MIPS adapter supplies an `accept`/`fcntl`
compatibility wrapper; its descriptor flag update is valid for this single-threaded
process, which does not concurrently fork. Existing factory sleep/name shims remain.
With strict request decoding the package is 1,343,120 stripped bytes, static
big-endian MIPS-I/o32. The initial status-only checkpoint was 1,228,412 bytes;
JSON/body support adds 114,708 bytes. These are package sizes, not RSS or
physical-device runtime measurements. ELF validation is recorded in the native plan.
