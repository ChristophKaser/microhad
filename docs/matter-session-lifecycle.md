# Commissioning session lifetime

`sessions::Sessions` is a single-owner manager for one active job and one
retained terminal snapshot. It owns the pinned commissioning future and borrows
the stable gateway BTP relay. It is not wired into production IPC yet: start
must remain unavailable until persistent fabric identity, Thread readiness and
the actual commissioner task are connected.

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
