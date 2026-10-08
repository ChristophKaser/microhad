# Commissioning session lifetime

## Cancel when the IPC peer disconnects — 2026-10-05

The controller now checks the selected commissioning event peer on every
publish turn. When that IPC connection closes, it cancels the active session
and drops its commissioner task immediately. This also covers a lost start
response where the native worker never learned the session ID; the worker's
cancel-on-reconnect path handles sessions whose IDs it already received.
Reconnection starts a fresh attempt with a new BLE generation; old requests
remain stale and mutations are never replayed.

`python3 thread-matter/scripts/test-matter-service.py` passes with a real `thread-matter`
host process and synthetic RCP. The test drops the IPC peer while a waiting-BLE
session is active and observes its `cancelled` state over a replacement peer.
`bash thread-matter/scripts/build.sh host service` and
`bash thread-matter/scripts/build.sh mips service` pass; the stripped MIPS
service is 2,158,276 bytes. No gateway process or radio was used.

`sessions::Sessions` is a single-owner manager for one active job and one
retained terminal snapshot. It owns the pinned commissioning future and borrows
the stable gateway BTP relay. The production `controller::Controller` now
connects it to IPC, persisted authority/allocator and the actual commissioner.
Start remains `not_ready` until a validated dataset is active on a freshly
observed matching coordinator channel.

Session IDs are unpredictable nonzero OS-entropy u64 values, rendered as sixteen
lowercase hex characters. They are distinct from IPC correlations. Status uses
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

Five host lifecycle tests cover busy/start/cancel/new-session races, dropping
an owned pending future, deadline expiry, reconnect and retired progress,
stale generations and sequence/write ordering, explicit task completion/error,
backwards progress and failed GATT writes. These run alongside the existing
actual BTP fragmentation checks at MTUs 23 and 247. The combined service-feature
library suite now has 27 passing tests. Synthetic future completion in these
tests is not accessory commissioning evidence; encrypted PASE/CASE fixture
coverage remains documented separately.

## Production owner checkpoint — 2026-10-04

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
queue. Missing IPC peers now cancel their active commissioning session;
write/session deadlines remain additional bounds. Reconnection must attach a
new BLE generation when a pending write was lost.
Native persistent event consumption remains to be implemented.

Evidence: 42 service-feature tests, including production-owner readiness/no-write
status/cancel/deadline checks; 24 control-only tests; actual running-service
start, busy, status, attach/write event, reconnect, stale callbacks, cancellation,
failed write and real 30-second timeout all pass. The encrypted accessory fixture
also completes through this owner/future, then proves CASE controls and authority/
fabric recovery after the phone is detached. The fixture retains synthetic GATT
and explicit fixture-only rendezvous, so it does not prove production DNS-SD.
Production completion must still wait for actual operational discovery; it
never injects fixture addresses or returns simulated success. Remote catalog recovery and
report integration remain in plan 07.
