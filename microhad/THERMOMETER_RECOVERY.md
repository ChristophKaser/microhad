# Küche TH01 recovery — 2026-10-03

> **Fix checkpoint (2026-10-03):** native now matches the current herdsman TH01
> profile. The fresh-join replay defect was reproduced and fixed on the host;
> both verified MIPS executables are deployed. Device acceptance and sustained
> reporting await user availability; temporary captures are stopped.
> [Implementation, validation and current plan](TH01_HERDSMAN_FIX.md).

> **Herdsman comparison (2026-10-03):** hardware failure is not established.
> The herdsman profile is now deployed. The fresh-join replay defect is
> reproduced and fixed on the host; its role in the live failure remains unverified.
> [Profile, lifecycle, evidence and next steps](HERDSMAN_TH01_COMPARISON.md).

## Current state and plan

The latest reporting investigation found a third bug: a known device rejoining
with cached endpoints queued only attribute reads, skipping reporting bindings
and Configure Reporting. A read-only refresh already in progress could also
prevent configuration. The correction is verified for both native
backends, committed as d33899e, and deployed. Küche accepted all three bindings and reporting configurations, and sent
unsolicited temperature/humidity reports. The 15-minute window is complete:
four early reports, no later updates after a subsequent rejoin, and no gateway
offline samples. Sustained reporting remains unresolved; successful attribute reads alone do not prove reporting is configured.

Two earlier confirmed bugs are fixed and host-tested: relayed own parent announcements
could delete restored sleepy children in Ziggurat, and the native parser rejected
signed humidity readings from eWeLink TH01. Both fixes are deployed. The restored child survives fixed production startup;
a fresh post-fix signed humidity read is now accepted, but the thermometer
still did not sustain reports after the later rejoin. Preserve current private state; no device removal, radio flash,
network recreation or security reset is needed.

The real trace already received temperature 21.82°C, humidity 83.53% and battery
100% from Küche. The original dashboard still showed humidity zero because it
rejected that read-response type. Automatic reports were received after configuration; continued reporting
after sleep remains unresolved. The other TH01 continues reporting normally.

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
user's wake attempt. All three bindings and reporting configurations were accepted, and automatic
reports were received; the completed window did not establish sustained reporting.

## Accepted configuration and real reports

At gateway timestamps 1791024388–1791024389 Küche returned three successful
Bind responses (status 0x00). At 1791024392–1791024395 it accepted Configure
Reporting for temperature, humidity and battery (all status 0x00). No signed
humidity configuration fallback was needed: standard uint16 reporting setup
was accepted, and its subsequent humidity reports use uint16, even though
its Read Attributes response uses int16. The corrected parser accepted a
fresh read of 81.11% humidity.

Unsolicited Report Attributes frames followed at 1791024394: 22.29 °C and
85.74% humidity. Further humidity reports at 1791024424 and 1791024454
reported 74.75% and 72.55%, confirming 30-second change-triggered reporting.
Battery readback was 100%. A later announcement at 1791024496 was followed
by an interview send failure at 1791024506; do not treat the early success
as proof of sustained operation after this later rejoin. The completed 15-minute
observation window from the first report received only the four early reports.

A read-only Read Reporting Configuration attempt after the wake yielded no
responses in its bounded 35-second window; exact stored intervals have not
been independently read back. The gateway maintenance COBS bridge has an
intentional 120-second alarm, so the initial COBS trace closed before this
wake. Real acceptance/report evidence above comes from the retained native
RAM log, journaled privately on the host. The continued window uses HTTP
status and log snapshots, without extra data written to device flash.

The user's requested action alerts use POST `/api/notifications/test`,
which queues the gateway's existing generic push notification. The pairing
alert request was accepted and logged at 1791024464. This endpoint has fixed
text; the specific action is explained in chat. Do not claim REST acceptance
as proof of handset delivery. No additional pairing action is currently
requested while the reporting window runs.

## Reproducible diagnostic tools and commands

The ad hoc host trace, readback, observation, and REST-alert scripts used during
this investigation are consolidated in `scripts/diagnose_ziggurat_reporting.py`.
Its five host tests cover captured-report parsing, truncated frames, manufacturer
headers, reads versus unsolicited reports, IEEE byte order, and explicit-action
guards. Trace mode rotates the maintenance bridge before its 120-second alarm.
Observe mode counts only Kitchen's unsolicited report log entries and records
network readiness; readback mode never sends binding/configuration writes.
It uses the already tracked `network_backup.py` SSH helper and
`ziggurat_cobs.py` codec/client. No debugging process is installed on the gateway.

Prerequisites: Python 3; `ssh`/`sshpass` for COBS modes; password file outside
Git; the deployed maintenance bridge and existing COBS socket. Store all output
in a directory with mode 0700; the tool creates new files with mode 0600 and
refuses overwrite. Never commit raw captures, device state, or credentials.

```sh
cd microhad
python3 scripts/test_diagnose_ziggurat_reporting.py
mkdir -m 700 /tmp/thermometer-diagnostic

# Read-only 15-minute status/log journal. --since can include retained earlier
# traffic; otherwise the window starts now.
python3 scripts/diagnose_ziggurat_reporting.py --host 192.168.178.62 \
  --mode observe --duration 900 \
  --output /tmp/thermometer-diagnostic/observation.jsonl

# Read-only APS capture. Socket bridge sessions rotate before 120 seconds.
python3 scripts/diagnose_ziggurat_reporting.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --mode trace --duration 900 \
  --output /tmp/thermometer-diagnostic/aps.jsonl

# When the device is awake: read its stored reporting configuration.
python3 scripts/diagnose_ziggurat_reporting.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --mode readback --duration 35 --execute \
  --output /tmp/thermometer-diagnostic/readback.jsonl

# Only when asking the user for a pairing attempt: opens the existing join
# window and queues the existing generic notification, as explicitly requested.
python3 scripts/diagnose_ziggurat_reporting.py --host 192.168.178.62 \
  --mode alert --execute --output /tmp/thermometer-diagnostic/alert.jsonl
```

The original COBS trace had the 120-second bridge limit and yielded no frames
before the physical wake; the native RAM log contains the successful sequence.
The readback command above consolidates the same three ZCL 0x08 requests
(temperature attribute 0x0000, humidity 0x0000, battery percentage 0x0021).
Its asynchronous 0x09 responses are recorded separately from send admission.
The tool's 30-second read-only production trace validation completed normally.

For rollback evidence and deployment, the existing tracked tools were used:

```sh
# Set this to a NEW directory for each maintenance operation.
mkdir -m 700 /tmp/thermometer-maintenance
# SSH calls use network_backup.ssh_args and the password-file pathname, never
# a password literal. Quiesce with /tuya/native/paired_control.sh quiesce.
../tools/network-migration-venv/bin/python \
  scripts/ziggurat_freeze.py --template /private/final-normalized-state.json \
  --host 192.168.178.62 --password-file ../ssh_password.txt \
  --bridge /tuya/native/gateway-maintenance --quiesced --execute \
  --output /tmp/thermometer-maintenance/frozen.json
# After snapshot success, stop with paired_control.sh stop
# --frozen-snapshot-saved and wait for gateway-maintenance check-free /dev/ttyS1.
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native --binary build/mips-ziggurat/microhad --binary-only
# Start /tuya/tuya_start.sh through SSH; verify /api/status readiness and
# inventory. The previous executable is archived on the host, not copied
# again into scarce device flash. The existing final-state rollback procedure
# in ZIGGURAT_DEPLOYMENT_PLAN.md remains authoritative.
```

Other validation commands used: both `make host test`/`make mips` and
`make ZIGGURAT=1 host test`/`make ZIGGURAT=1 mips`, followed by
`python3 scripts/test_ziggurat_service.py` (nine real-server lifecycle scenarios).
SSH `df -k /tuya`, `md5sum`, and `ps w` verified space, executable identity
and the production pair. Read-only ScanChildren (COBS 0x22) and address-cache
(0x23) checks verified device mappings without starting/reconfiguring the stack.

A child-table caveat remains: the pre-deployment frozen snapshot had the child,
but a running scan immediately before the fresh wake returned zero children.
After the successful association, the scan returned the expected child again.
The earlier own-parent guard remains deployed, but this additional disappearance
is not yet explained. Do not claim long-lived child restoration proven from
one successful startup or association. Retain snapshots and use poll/parent
traces if reports stop after sleep.

## Final bounded observation and remaining uncertainty

The 900-second window started at 1791024394 and ended at 1791025294.
There were four unsolicited reports total (one temperature, three humidity),
all within the first minute. No later Küche reports followed its announcement
at 1791024496. The host sampled 32 times from attachment onward, with zero
offline/network-unformed samples; retained RAM logs supplied the earlier
report evidence. The comparison Bad Thermometer continued to report.

Thus the missing configuration path is fixed and hardware acceptance was
verified, but this is not a complete thermometer recovery. A thermometer
fault is plausible given its pre-Ziggurat history and failure to sustain
updates. It is not proven: a reset/rejoin can change device reporting state,
and the unexplained child-table discrepancy leaves gateway behavior as an
alternative. No fresh reporting-configuration readback was obtained after
the later rejoin. Last observed values were 22.29 °C, 72.55% humidity, 100%
battery; the percentage is a device reading, not a battery load test.

Next, if the user wants another check: verify battery/contact condition,
briefly wake without a factory reset, capture polls and stored reporting
configuration while awake, then observe sleep without another pairing attempt.
Use the requested gateway REST notification only when an action is actually
needed, and explain the specific action in chat. No new action is requested
now. The extra read-only trace attached for a possible wake was stopped;
no host diagnostic or extra production process remains. Production remains
on the committed reporting fix with the existing radio/network intact.
