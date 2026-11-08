# Commissioning session lifetime

The current service is `mthread-matter` in `micro-thread-matter-daemon/`, using
[protobuf IPC v3](../micro-thread-matter-daemon/docs/matter-ipc-v3.md). The service owns the session lifecycle described here.

## Disconnect and cancellation

The controller checks the selected commissioning event peer on every
publish turn. Before Thread handover, closing that IPC connection cancels the
active session and drops its commissioner task immediately. Once successful
Thread provisioning advances to `case`, loss of the phone or IPC peer leaves
CASE and CommissioningComplete running under the original session deadline. This also covers a lost start
response where the native worker never learned the session ID; the worker's
cancel-on-reconnect path handles sessions whose IDs it already received.
Reconnection starts a fresh attempt with a new BLE generation; old requests
remain stale and mutations are never replayed.

`sessions::Sessions` is a single-owner manager for one active job and one
retained terminal snapshot. It owns the pinned commissioning future and borrows
the stable gateway BTP relay. The production `controller::Controller`
connects it to IPC, persisted authority/allocator and the actual commissioner.
Start remains `not_ready` until a validated dataset is active on a freshly
observed matching coordinator channel.

Session IDs are unpredictable nonzero OS-entropy u64 values, rendered as sixteen
lowercase hex characters in public/fixture JSON and unsigned integers on the
private protobuf wire. They are distinct from IPC correlations. Status uses
monotonic deadlines (30–900 seconds); reading status or accepting callbacks does
not extend them. Cancellation, deadline expiry and task completion drop the
owned future before clearing BTP, invalidate the connection generation, and
clear the retained passcode. The manager and each attempt hold zeroizing PIN
storage. A bounded PASE reconnect drops the old task before replacing BTP and
creates a new progress cell so retired attempts cannot publish state.

States progress through `waiting_ble`, `pase`, `provisioning`, `waiting_thread`,
`case`, and `completing`, then `complete`, `cancelled`, `failed` or `timed_out`.
The actual commissioner publishes progress; a task can cross several phases in
one poll. Backwards or premature terminal progress fails closed. Success requires
the completing phase and a nonzero node ID. Crypto polling also rechecks the
wall-clock deadline before accepting completion.

All callbacks check both session and generation before touching BTP. Indication
sequence starts at one and must increase exactly once per accepted indication.
MTU limits are checked before processing input. Only one C1 write is outstanding;
a completion must match its ID. A wrong/stale completion leaves the new
connection intact; a failed or timed-out write terminates the attempt. The relay
checks its ten-second write deadline without consuming another outgoing packet.
Generation overflow fails closed instead of wrapping. There is no unbounded
session, attempt or credential queue.

## Commissioner ownership

IPC opcodes 3–8 dispatch start/status/cancel, attach, indication and write
completion. Start returns the waiting-BLE snapshot without allocating a node or
writing credentials. Attach creates and installs the actual pinned commissioner
future; its first poll persists a unique node allocation before PASE/fabric
provisioning. Reattach during PASE drops the old future and authority borrow
before resetting BTP and creating a new attempt. Allocator gaps remain deliberate.

The future uses the production signer and owner's attestation policy, provisions
the validated active dataset over authenticated PASE and completes over CASE.
The service pumps BTP and userspace UDP in one chained Matter transport. Losing
Thread readiness fails an active job with `not_ready`; deadline expiry takes
precedence when both happen at the same poll. Terminal states drop the task
and zeroizing passcodes before invalidating callbacks.

Start/attach select the persistent IPC event peer. Valid status with optional
`subscribe:true` can select a new peer; status without it does not retarget.
Commissioning snapshots and opaque C1 writes use the targeted bounded event
queue. Missing IPC peers cancel commissioning only before `case`. Before that
phase, BLE write deadlines remain additional bounds and reconnecting requires
a fresh BLE generation. From `case` onward, no new C1 writes are scheduled,
current-generation late BLE callbacks are ignored, and an unconfirmed final BTP
ACK cannot expire the commissioning future. Explicit Cancel and the overall
session deadline remain effective. The app falls back to read-only HTTP status
polling once per second after relay loss during Thread verification; failed
polls do not extend that deadline.
The native persistent worker consumes these events and retires relay ownership
on disconnect. Verify with the synthetic service and native worker fixtures;
the deployed stack can also be checked for phone loss and service/radio restart.
