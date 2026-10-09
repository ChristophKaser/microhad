# HDLC multi-PAN implementation plan

> **Current fix and plan (2026-10-03):** the herdsman TH01 profile and scoped
> fresh-join replay fix are host-tested and deployed. The replay defect failed
> before and passes after the patch in the actual host server. Production is
> online; joining is closed and temporary captures are stopped. Device acceptance
> and sustained reporting await user availability. No radio flash was performed
> for these fixes. Next: capture, REST alert, one pairing attempt, then observe.
> [Implementation, tests, deployment and rollback](th01-herdsman-fix.md).

> **Herdsman review (2026-10-03):** hardware failure is not established.
> The comparison led to the deployed profile and reproduced replay fix above;
> its role in the real thermometer failure still needs a live capture.
> [Comparison and next software tests](herdsman-th01-comparison.md).

> **Reporting correction (2026-10-03):** cached-endpoint rejoins previously
> skipped binding/reporting setup. The native fix is tested for both backends
> and deployed. Küche accepted setup and sent four reports, but sustained
> reporting after a later rejoin remains unresolved.
> [Current thermometer status](thermometer-recovery.md).

> **Post-deployment fixes (2026-10-03):** encrypted host traffic reproduced
> loss of a restored sleepy child on a relayed own parent announcement. Fixed
> Ziggurat and TH01 signed-humidity parsing are deployed; the child survives
> startup. [Trace, evidence and remaining verification](thermometer-recovery.md).


## Current state and plan — 2026-10-03

**Zigbee production deployment is complete.** The two-instance MG1 HDLC RCP was
flashed application-only on the existing Tuya. Patched MIPS Ziggurat owns IID 1;
the native client uses COBS over a private Unix socket and owns durable state.
All 12 migration steps, permanent supervision, automatic reboot recovery and
final frozen recovery backup preparation are complete. Existing lights and
measurement reports work. Thread IID 2 is unused. See
[deployment/rollback](ziggurat-deployment-plan.md),
[hardware measurements and limits](real-radio-migration.md) and
[native protocol/state](native-ziggurat-integration.md).

The firmware, patches and build commands below remain the reproducible inputs.
Owner reset and bootloader entry address IID 1; IID 0 is reserved for broadcast
notifications in this SDK. Host/simulator checks now reject a reset to IID 0.
The UART helper's reverse bootloader entry/run passed on hardware, while no live
reverse NCP restoration was performed. Original backups and fresh frozen exports
remain private and outside Git.

Sleepy-child continuity is physically unverified (user waived stimulation),
broadcast relay-quorum/CCA verdicts remain intermittent, and cold-boot readiness
took 121 seconds. RNG blocking is suspected, not established. Crash recovery
requires sufficiently fresh state; the restart margin is not an unconditional
guarantee. Production memory after reboot had 12844 KiB free out of 27892 KiB.

## Remaining plan

1. Investigate Configure/cold-boot readiness and verify sleepy devices.
2. The [OpenThread Unix adapter and MIPS CLI proof build](openthread-feasibility.md)
   now pass host transport tests, ELF checks and a bounded MIPS simulation with
   internal UDP self delivery. Choose direct internal UDP or
   resolve missing kernel IPv6/TUN for Linux routing, then validate startup, ABI, reset,
   common channel, scans and combined resources. IID 2 remains unused in production.
3. Validate simultaneous PAN ACKs/polls, overlapping addresses and scan interference.
4. Add standalone Matter and benchmark the complete gateway.

No cpcd or spare gateway is required. Current deployment is Zigbee-only.
Historical pre-flash preparation below explains backup design; the current
execution evidence is in the linked deployment plan and hardware findings.

## Network preservation before flashing

Retaining existing device pairings may be possible, but is **not yet verified
for this gateway and Ziggurat**. Switching from EZSP NCP to raw HDLC RCP moves
the Zigbee stack from the radio into Ziggurat. The RCP does not run the old
Zigbee stack or automatically consume its saved network tokens; the existing
native service must be explicitly built with its optional Ziggurat backend.

The native `network.json` remains insufficient by itself. The new fixed-command
export runs through the existing UART owner and captures identity, network and
trust-center key material, NWK/APS counters, key-table slots and direct children.
The installed NCP omits the token-interface library (`0xb5`), so this is a
logical migration backup, not a complete token or flash dump.

Completed without flashing:

1. Identified live EmberZNet 7.5.1.0 / EZSP v13 and obtained a fresh private
   logical backup through the deployed native service. Earlier coordinator
   identity, PAN/channel, network key and trust-center seed match.
2. Converted state to standard coordinator JSON and a Ziggurat protocol-v2
   import bundle. Actual state round-trips through a stopped host Ziggurat
   stack, including its direct child; zero frames are transmitted in that test.
3. Added `FreezeNetwork` (`0x04`) to stop tracked tasks while retaining final
   state. A synthetic running network freezes with stable counters; restart and
   key mutation are refused. Orderly rollback requires this final frozen state
   and advances both NWK and APS counters with overflow/identity checks.
4. Updated the guarded NCP restore tool to advance and verify APS as well as
   NWK counters. Mocked restoration tests pass; no live restoration was executed.
5. Validated the retained, previously installed NCP GBL's CRC, SHA256 and
   application-only range. RCP NVM occupies `[0x37000,0x40000)` (36 KiB), and its
   strong Spinel-to-Gecko bootloader entry implementation is linked.

Preparation requirements (now see completed deployment evidence above):

- Follow the same-device bootloader entry, flashing and logical restoration
  checkpoints, including the limits of recovery from an unresponsive application. The compiled
  Spinel recovery path needs a responding RCP; no verified SWD fallback exists.
- Validate the implemented native adapter and ZHA-style checkpoint persistence
  on hardware. It does not reserve durable upper bounds for every future counter;
  crash rollback without sufficiently current state is not established. Take a
  new backup immediately before handover.
- Validate real device pairing preservation and dual-PAN traffic. Identical NVM
  allocation alone does not establish storage compatibility or functional recovery.

See [commands, private artifacts and limitations](network-migration.md).
Silicon Labs' documented
[NCP-to-host token migration](https://docs.silabs.com/zigbee/9.0.0/multiprotocol-solution-linux/host-ncp-rcp-migration)
requires token-export support absent from this installed NCP; our implemented
path uses logical metadata and key export instead. The native service was
updated/restarted with authorization; radio firmware was not replaced.

## Firmware prerequisites

The firmware build runs on an **x86_64 Linux development computer**. The
installer downloads the x86_64 ARM compiler; it does not run on the Tuya.
Allow about **10 GiB free disk space** for downloads, SDK, tools and outputs
(the installed Silicon Labs tools currently occupy about 5.5 GiB).
Internet access and GitHub access to this private repository are required.

Required system tools are Bash, Git, **Git LFS**, curl, CA certificates, tar,
xz, Python 3, GNU make, ripgrep and **Java 17**. On Ubuntu/Debian:

```sh
sudo apt-get update
sudo apt-get install git git-lfs curl ca-certificates tar xz-utils \
  python3 make ripgrep openjdk-17-jre-headless
```

`firmware/scripts/silabs-env.sh` selects `/usr/lib/jvm/java-17-openjdk-amd64/bin`.
Adjust that path on a different distribution. Ziggurat needs Rust and a
selected gateway toolchain (RSDK or experimental crosstool-NG); see
[toolchain selection](../config/TOOLCHAINS.md). Neither is required for this ARM
firmware build.
The installer supplies pinned **GSDK 4.5.0**, **SLC 5.6.3** and
**ARM GCC 12.2.rel1** under ignored `tools/silabs/`.

## Commands to build and verify firmware

Run from the experiment repository, **not** its parent `../`:

```sh
cd .
git submodule update --init --recursive
bash firmware/scripts/install-silabs.sh

# Ensure SDK binary libraries are downloaded, rather than LFS pointer files.
git -C firmware/dependencies/gecko-sdk lfs install --local
sdk_lfs_libraries='platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a'
sdk_lfs_libraries+=',protocol/openthread/libs/libsl_openthread_efr32mg1x_gcc.a'
sdk_lfs_libraries+=',platform/radio/rail_lib/autogen/librail_release/librail_efr32xg1_gcc_release.a'
git -C firmware/dependencies/gecko-sdk lfs pull --include="$sdk_lfs_libraries" --exclude=''

BUILD_JOBS=4 bash firmware/scripts/build-hdlc-firmware.sh
```

Use Bash for this sequence (`+=` appends to the LFS path list). On another
computer, replace the `cd` path with your checkout directory. The build script
loads the tool environment itself, trusts the pinned SDK, generates the
project, compiles it and verifies both multi-instance initialization symbols.
It does not access the gateway or flash firmware.

Outputs are in `build/firmware-hdlc/build/debug/`:

- `ot-rcp-multipan.out`: ELF firmware.
- `ot-rcp-multipan.bin`: raw application binary, **Flash base `0x4000`**.
- `ot-rcp-multipan.map`: Flash/RAM allocation map.

Generation/build logs are `logs/hdlc-firmware-generate.log` and
`logs/hdlc-firmware-build.log`. A bootloader-update **GBL is not produced**;
the raw binary is not a complete chip image. Firmware remains unflashed.

To inspect the output independently:

```sh
source firmware/scripts/silabs-env.sh
arm-none-eabi-size build/firmware-hdlc/build/debug/ot-rcp-multipan.out
arm-none-eabi-nm build/firmware-hdlc/build/debug/ot-rcp-multipan.out \
  | rg 'otAppNcpInitMulti|otNcpHdlcInitMulti'
```

For Ziggurat/MIPS build commands and the bounded simulator test, see
[HDLC multi-PAN usage](hdlc-multipan-usage.md). Pushing documentation from the
parent workspace requires the repository path:

```sh
git -C . push
```

## Completed initial implementation scope

Started 2026-10-02. The following criteria describe the initial implementation;
the remaining production work is listed above.

1. Build a disposable MG1 two-instance HDLC firmware project using pinned
   GSDK 4.5.0 and the gateway board configuration. Keep upstream submodules
   unchanged. Record initialization, binary dependencies, Flash/RAM usage and
   any unresolved build failure. Do not flash the production radio.
2. Patch Ziggurat's Spinel transport with independently tracked interface
   clients sharing one HDLC reader/writer. Reject cross-interface responses;
   allow one reset owner and notify all interfaces on a physical reset.
   Preserve ordinary IID 0 operation for the existing single-PAN simulator.
3. Expose IID selection in the server and an in-process interface for a second
   local consumer. Test simultaneous same-TID requests, unsolicited receive
   isolation, reset cancellation and serialized framing over simulated I/O.
4. Extend simulation to multiple independent PAN/address/source-match states,
   then cross-compile and harmlessly test against host simulation on the Tuya.
   Do not access `/dev/ttyS1`, production state or production services.
5. Document actual results, remaining OTBR/Matter integration and hardware
   gates. An exposed Thread interface is not a working Thread border router.
6. Publish this experiment's reproducible source, scripts, patches and docs
   to a private GitHub repository. Exclude credentials, tool downloads, local
   logs, build outputs and production configuration.

## Implementation and validation history

- Plan recorded before implementation.
- Existing source audit: [radio-dual-mode.md](radio-dual-mode.md).
- Existing Ziggurat/MIPS and CPC experiments remain separate baselines.
- GitHub authentication checked: account `ChristophKaser`.
- Private repository created and privacy verified:
  https://github.com/ChristophKaser/ziggurat-compilation.
- Two-instance HDLC firmware compiled successfully with GSDK 4.5.0, ARM GCC
  12.2.rel1 and Silicon Labs' published SLC 5.6.3 package. Both
  `otAppNcpInitMulti` and `otNcpHdlcInitMulti` are linked. Application Flash
  starts at `0x4000`; the map fits its configured Flash/RAM regions.
- Shared reader/writer, per-interface transactions and receive routing,
  reset ownership, server IID selection and a local Thread HDLC endpoint are
  implemented in a disposable worktree and recorded as a patch.
- Ten existing Spinel tests plus five new multi-PAN integration tests pass.
  Seven Python simulator checks also pass. The actual host-server/two-PAN
  simulation passes all eight scenario checks.
- The MIPS binary builds. The first device simulation passed configuration,
  isolation, reset/channel rejection, receive and mixed-query checks. The
  recovery test initially expected an extra reset instead of reprogramming;
  that assertion is corrected. A subsequent upload timed out, followed by
  unresponsive SSH/dashboard; RAM-backed uploads were a suspected cause.
- The user power-cycled the gateway. SSH and HTTP recovered. `/tmp` is ramfs,
  so the harness now requires genuinely free RAM, reserves an 8 MiB margin
  beyond uploaded file sizes and cleans every experiment file afterward.
- The final safeguarded device test **passes all eight checks**. It started
  with 17,300 KiB free against an 11,659 KiB requirement. The running Ziggurat
  reported 2,068 KiB RSS; this excludes an actual Thread/Matter stack.
  Thirty Thread-interface queries had median 7.21 ms and p95 7.82 ms,
  including the SSH test bridge. Cleanup left no test files/processes; the
  production dashboard returned HTTP 200 afterward.
- One cold-boot attempt stalled during Zigbee configuration. The Rust RNG
  fallback waits for `/dev/random` readiness; low startup entropy is the
  suspected cause. A later run succeeded after the entropy pool recovered.
  RNG safety checks were retained. Reliable cold-boot startup is still a gate.
- Firmware remains unflashed. OTBR transport integration, local Thread/Matter
  builds, real radio ACK/source-match behavior and joint resource use remain
  future work, as explicitly scoped in this plan.
