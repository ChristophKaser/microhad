# Küche TH01 recovery — 2026-10-03

## Current state and plan

The latest reporting investigation found a third bug: a known device rejoining
with cached endpoints queued only attribute reads, skipping reporting bindings
and Configure Reporting. A read-only refresh already in progress could also
prevent configuration. The correction is verified for both native
backends, committed as d33899e, and deployed. Hardware acceptance and unsolicited reports remain
pending; successful attribute reads alone do not prove reporting is configured.

Two earlier confirmed bugs are fixed and host-tested: relayed own parent announcements
could delete restored sleepy children in Ziggurat, and the native parser rejected
signed humidity readings from eWeLink TH01. Both fixes are deployed. The restored child survives fixed production startup;
a fresh post-fix humidity read and sustained reporting are still being checked. Preserve current private state; no device removal, radio flash,
network recreation or security reset is needed.

The real trace already received temperature 21.82°C, humidity 83.53% and battery
100% from Küche. The original dashboard still showed humidity zero because it
rejected that read-response type. Continued automatic reporting remains to be
checked after deployment. The other TH01 continues reporting normally.

## Evidence and causes

Earlier attempts created a direct sleepy child, but native interview sends failed
about ten seconds later. The targeted driver trace logs only poll addresses,
known-child status and pending/delivery decisions; it never prints frames or keys.
During a fresh association the device polled with its IEEE and short addresses,
source-match updates succeeded and queued replies were extracted. Temperature,
humidity and battery read responses arrived. No transport behavior was changed
by the diagnostic patch, so this rules out a blanket inability to serve TH01 polls;
it does not establish the cause of every earlier unsuccessful attempt.

The child disappeared immediately after orderly restart. Native persisted one
child, but the running stack exported none. Logs showed a received Parent_annce
(cluster 0x001f) with NWK source 0x0000 containing that child after both restarts.
A router relayed our own broadcast using its MAC source. The NWK filter stopped
some routing work but its verdict did not stop subsequent APS/ZDP dispatch;
ZDP treated the source-zero announcement as another parent's claim. Restored
children have not yet confirmed a keepalive, so the neighbor routine removed it.

ZDP now ignores Parent_annce and Parent_annce_rsp whose NWK source is our own
address. Other routers' legitimate claims still work. The host regression uses
actual native service, Ziggurat and simulator with encrypted synthetic NWK traffic.
It fails on the earlier binary at the lost-child assertion, then passes with the
fix for self announcements/responses and another-router removal. Broader NWK/APS
deduplication behavior is outside this narrow fix.

The captured humidity read response used ZCL int16 (0x29), despite the standard
unsigned humidity type 0x21. Its value 0x20a1 is 8353 hundredths. The parser now
accepts this encoding only for manufacturer eWeLink / model TH01. Both encodings
remain limited to 0..10000; negative, invalid, overrange and truncated values are
rejected. Standard unsigned readings and unrelated devices retain validation.

## Verification and recovery

Native `make host test`, `make mips`, `make ZIGGURAT=1 host test` and
`make ZIGGURAT=1 mips` passed after the parser change. Regression tests cover the
captured value, strict unknown-device rejection, range/truncation and standard
encoding. Nine actual native/stack/simulator lifecycle scenarios pass including
the new encrypted parent test. Poll diagnostics also passed eight HDLC host
scenarios and seven COBS host scenarios; driver compile check had zero unit tests
and is not counted as behavior verification.

Before trace/deployment, quiesce and save complete private frozen snapshots.
When repairing the child entry deleted by this bug, use its successful post-trace
record only after checking the current IEEE/address mapping and network/security
identity. Keep latest counters and keys, full latest address table, and the native
startup counter margin. Save the repair as a separate private artifact; do not
restore old counters or revive every historical child indiscriminately.

The extra executable copy initially exhausted flash during an atomic upload.
Installed binary remained intact. The redundant copy was archived and verified
on the host, and only our incomplete staging/duplicate files were removed. Trace
output streamed through SSH into a bounded private host file, never flash. Normal
paired supervision resumes with diagnostic logging disabled.

## Fixed production startup

Before deployment, a fresh complete frozen snapshot was saved. The single child
verified in the successful trace was restored after identity/security and current
address-map checks. Latest counters/keys and all current address entries were
preserved. Both binaries were atomically replaced and normal paired supervision
resumed without radio flashing. The running stack then exported the expected
sleepy child, whereas the earlier binary exported none after startup.

Installed MD5s: native `6f462ee32142e5510e3def94446fc415`,
Ziggurat `53046d62702c6b06f06bc1ef7bf01f0b`. Host-held original executables,
frozen snapshots and repaired state are private and Git-ignored. Diagnostic
logging is disabled in normal production; the added debug line is dormant.

## Reporting configuration correction

A full interview of a known device now uses cached descriptors to bind its
sensor clusters and configure reporting, rather than running the read-only
refresh plan. If a rejoin overlaps a refresh, the refresh finishes with its
existing transaction and then starts configuration. Routine/manual measurement
refreshes remain read-only. Regression tests check cached rejoin, paused refresh
promotion, preservation of the in-flight transaction, and ordinary refresh
completion for both EZSP and Ziggurat builds.

The intended TH01 setup binds endpoint 1 temperature (0x0402), humidity
(0x0405), and battery (0x0001) to coordinator endpoint 1. Temperature reporting
uses int16, minimum 30 seconds, maximum 300 seconds, change 0.20 °C. Humidity
uses the standard uint16, minimum 30 seconds, maximum 300 seconds, change 1%.
Battery percentage uses uint8, minimum 3600 seconds, maximum 65000 seconds,
change 0.5 percentage points. Some TH01 read responses use signed humidity;
a successful humidity Configure Reporting response must be checked rather
than assuming that read-response quirk also applies to configuration.

Verification requires accepted Bind responses and Configure Reporting responses
from Küche, followed by unsolicited Report Attributes (ZCL command 0x0a).
Read Attributes responses (0x01), operation admission, and interview completion
alone are insufficient. Rejected configuration is currently logged and the
interview advances; inspect status codes explicitly. Preserve the live network
and snapshot before updating only the native executable. No radio firmware
change is required.

Reporting fix validation: `make host test`, `make mips`,
`make ZIGGURAT=1 host test`, and `make ZIGGURAT=1 mips` all passed, as did
the nine actual-server host lifecycle scenarios. Production native MD5 is
`cfea133ccc3f06d088facd95e5df783a`. Fresh private frozen state and the previous
executable were archived on the host before a clean pair restart. All 23
inventory entries remain and the network is formed. The radio image and
Ziggurat executable were unchanged. A bounded 15-minute host COBS capture
is observing device replies; a five-minute pairing window was opened for the
user's wake attempt. Hardware reporting verification remains pending.
