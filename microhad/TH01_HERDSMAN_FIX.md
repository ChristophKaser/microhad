# TH01 herdsman profile and fresh-join replay fix — 2026-10-03

## Current state and plan

Both fixes are implemented, host-tested and compiled for stock MIPS. Native
commit 8954776 matches current upstream herdsman's eWeLink TH01 settings/order
and fixes pending full configuration wakeups behind paused refreshes. Experiment
commit fe1a787 contains the Ziggurat replay patch and actual-server encrypted
regression. Both verified executables are deployed on the gateway. Readiness
checks confirmed online=true, network_formed=true and 23 inventory entries.
The five-minute joining window has closed and the temporary host capture has
been stopped. A fresh private frozen network snapshot and the previous
executable pair are archived on the host. Radio firmware, network identity
and existing device inventory are preserved.

The user is unavailable for a pairing attempt. The software profile is deployed,
but acceptance of the new reporting settings by the thermometer and sustained
reporting are **pending**, not confirmed. No action is required now.

Next, when the user is available: attach a bounded rotating host capture, send
the gateway REST alert, and request **one** pairing attempt. Inspect successful
Bind/Configure Reporting responses and unsolicited reports. Do not ask for
repeated resets while the sensor is already configured. One-hour reporting
maxima require a full hour or a controlled change-triggered report to assess
continued reporting; a quiet 15-minute window alone is not sufficient under
these new settings.

## Native profile

Only manufacturer eWeLink / model TH01 receives this profile. On endpoint 1:

1. Bind temperature (0x0402), humidity (0x0405), Power Configuration (0x0001).
2. Temperature attribute 0x0000: int16 0x29, min 30 s, max 3600 s, change 20
   (0.20 °C).
3. Humidity attribute 0x0000: uint16 0x21, min 30 s, max 3600 s, change 100 (1%).
4. Battery voltage 0x0020: uint8 0x20, min 3600 s, max 65000 s, change 0; read it.
5. Battery percentage 0x0021: uint8 0x20, min 3600 s, max 65000 s, change 0; read it.

There are no extra temperature/humidity reads between configuration writes.
Other devices keep their previous generic configuration. A paused refresh now
resumes on device traffic when configurationPending is true, so a queued full
configuration can finish; ordinary read-only refreshes remain quiet on reports.
Focused C++ tests check the full nine-operation TH01 plan, attributes, types,
intervals, changes, order, manufacturer scoping, and pending wake behavior.

## Confirmed replay defect and bounded fix

Before the patch, the actual host Ziggurat server accepted an encrypted report
with network counter 100. After an authorized fresh join, it rejected a new
report with counter 4. Its security log explicitly reported a rollback from
100 to 4. This reproduces a real stack defect; it is not yet proof that the
same defect accounts for every real-device failure.

NwkSecurity now has reset_incoming_counter_for_fresh_join(sender), which removes
only the incoming NWK baseline for that IEEE. The shared begin_join calls it
only after a fresh MAC association or a router-reported unsecured **new join**
has passed the permit-window checks. Secured and trust-center rejoins do not
call it. An authorized new join is the intended security-state replacement
boundary; replay checks are not disabled for ordinary received traffic.

The incoming counter belongs to the actual relaying IEEE. Other relayers and
all unrelated device baselines remain intact. Network key, key sequence and
coordinator outgoing frame counter are retained. Failed authentication does
not install a new incoming baseline.

ziggurat/scripts/test-fresh-join-replay.py uses only synthetic keys and a localhost RCP
with the actual server. A valid processing-fence frame ensures negative cases
were processed before assertions. It verifies initial acceptance, stale-frame
rejection, secured rejoin and trust-center rejoin preserving counters, denied
fresh joins preserving counters, authorized fresh joins accepting reset
counters, invalid MIC rejection, duplicate rejection after the new baseline,
unrelated relayer protection, wrong key-sequence rejection, and retained
network identity/outgoing counters. The baseline test failed before the patch
at the fresh-join report and passes afterward.

The direct-association path and router new-join path share begin_join. The
end-to-end regression exercises the router Update-Device path; the direct
path's permit guard was source-reviewed, not independently radio-tested here.
A legitimate parent change must still remove a child from its old parent's
table. Do not suppress such changes to make a restored-child count look stable.

## Verification and artifacts

All passed:

```sh
cd microhad
make host test
make mips
make ZIGGURAT=1 host test
make ZIGGURAT=1 mips

cd ..
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/host" cargo +nightly-2026-10-02 build \
  --manifest-path worktrees/ziggurat-multipan/Cargo.toml -p ziggurat-server
python3 ziggurat/scripts/test-fresh-join-replay.py
python3 ziggurat/scripts/test-hdlc-multipan.py
python3 ziggurat/scripts/test-cobs-unix.py
python3 ../microhad/scripts/test_ziggurat_service.py
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
```

The build script applies ziggurat/patches/ziggurat-fresh-join-replay.patch reproducibly
alongside existing multipan, migration, stock compatibility and diagnostic
patches. The pristine source checkout remains unchanged.

New artifacts:

- Native: 1504596 bytes, MD5 9e2a0a853419f3818409bc757cec7a85;
  SHA-256 380cbcebf06f18f64f403957e8f504aae81bc8dac1228c39acd7f24e4181c255.
- Ziggurat: 3565488 bytes, MD5 f92b4c90422968a1420e965f51cb7115;
  SHA-256 55473ed1f4ecf517e7276c76e8b796c7994e59a6225d97cba3510f5f52b4347e.

Old rollback executables: native MD5 cfea133ccc3f06d088facd95e5df783a,
Ziggurat MD5 53046d62702c6b06f06bc1ef7bf01f0b. Both are archived privately
on the host, avoiding extra copies on scarce device flash. Follow the existing
quiesce/freeze/final-counter rollback procedure in ZIGGURAT_DEPLOYMENT_PLAN.md;
never restart from stale pre-migration counters. The network snapshot was saved
before stopping the pair. The two replacements were staged atomically and verified by MD5 before
/tuya/tuya_start.sh started the pair. A subsequent read-only check verified
both deployed MD5 values again and confirmed the network remains online with
joining closed. No radio image was flashed during this update.

## Diagnostic tools and production commands

Use the existing scripts/diagnose_ziggurat_reporting.py modes (observe, rotating
trace, bounded readback, and explicit REST pairing alert). Raw state/captures
remain private and outside Git. The tools and commands are described in
THERMOMETER_RECOVERY.md. A gateway REST notification request is proof of queue
acceptance, not proof the handset displayed it.

Native-only transfer uses:

```sh
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native --binary build/mips-ziggurat/microhad --binary-only
```

Ziggurat transfer uses SSH stdin to a new /tuya/native/ziggurat.new, chmod 700,
then atomic rename; no SCP subsystem exists. Check flash space before transfer,
verify the checksum, and retain the old file on transfer failure. Preserve the
paired launcher, runtime configuration and radio image.

See HERDSMAN_TH01_COMPARISON.md for pinned upstream source revisions and
THERMOMETER_RECOVERY.md for the complete earlier evidence and limitations.
