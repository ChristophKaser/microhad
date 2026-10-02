# HDLC multi-PAN implementation plan

## Current state — 2026-10-02

**The initial implementation is complete and published privately.** The
MG1 two-instance HDLC firmware compiles; Ziggurat's shared transport and local
Thread endpoint work against a simulated RCP on both the host and stock Tuya.
All 8 device scenario checks pass, alongside 15 Rust and 7 Python tests.
The linked firmware fits the configured Flash/RAM regions and contains both
multi-instance initialization functions. **It has not been flashed.**

This is a transport prototype, not a working Thread border router or Matter
controller. Actual OTBR/Thread integration, `microhad-zigbee-native` integration,
Matter functionality and real-radio dual-network operation remain pending.
The goal is fully standalone on the Tuya, with Zigbee IID 1 and Thread IID 2
sharing one radio without cpcd. Common-channel operation, one reset owner and
MG1 source-match limitations remain constraints. Cold-boot RNG readiness also
needs investigation before production.

Source, patches and documentation are in the
[private repository](https://github.com/ChristophKaser/ziggurat-compilation).
[Usage/results](hdlc-multipan-usage.md), [build manifest](hdlc-build-manifest.json)
and [validation summary](hdlc-validation.json) record the evidence.

## Remaining plan

1. Investigate cold-boot RNG readiness; add bounded, explicit startup error
   handling without bypassing the RNG safety check.
2. Implement and test a local OTBR Unix-socket transport adapter or embedded
   Thread platform interface. Coordinate reset/recovery, scans and channel
   changes with Ziggurat; benchmark combined RAM and CPU on the Tuya.
3. Prepare firmware backup/recovery and a separately authorized hardware test.
   Complete the network-preservation gate below before using the production radio.
   Validate both PANs' ACKs, sleepy-child polls, address collisions, broadcast
   routing, scanning interference and sustained traffic on MG1.
4. Implement the `microhad-zigbee-native` protocol-v2 client and local Matter
   functionality; validate the full standalone system and resource budget.

## Network preservation before flashing

Retaining existing device pairings may be possible, but is **not yet verified
for this gateway and Ziggurat**. Switching from EZSP NCP to raw HDLC RCP moves
the Zigbee stack from the radio into Ziggurat. The RCP does not run the old
Zigbee stack or automatically consume its saved network tokens; the existing
native EZSP client also needs its planned Ziggurat adapter.

Source inspection of `microhad-zigbee-native/src/storage.cpp` (`saveNetworkState`)
shows that the native state file saves PAN ID, extended PAN ID, channel,
network key and preconfigured key. It does not save the operational coordinator
EUI64, key sequence, NWK/APS outgoing counters or device link-key table.
Copying this file alone therefore does not establish a safe migration.
Ziggurat's protocol has network-state and key-table restoration interfaces,
but our native export/conversion/import path has not been implemented or tested.

Before a production flash:

1. Identify the installed NCP firmware, token storage and available EZSP
   export commands. Obtain a protected backup through the UART owner without
   competing for `/dev/ttyS1`. Never commit network keys or token dumps.
2. Export and map the operational coordinator identity, PAN/channel settings,
   network key and sequence, trust-center security material (including any
   hashed-link-key seed), device keys and outgoing security counters. Evaluate
   child and other persistent tables supported by the installed stack.
3. Implement restoration into Ziggurat and persistence through the native
   client. Verify that transmitted security counters exceed previously used
   values; devices can reject packets when counters go backwards.
4. Check bootloader compatibility, flash/NVM erase boundaries and whether the
   RCP initializes overlapping storage. Establish an obtainable original NCP
   image or recoverable flash backup and a tested recovery method. Leaving
   token pages untouched alone does not preserve operation under an RCP.
5. Rehearse migration and rollback on a spare radio/network. Rollback after
   Ziggurat has transmitted must also account for counter advancement; an old
   firmware image and stale token backup are not a demonstrated rollback.

Silicon Labs documents an
[NCP-to-Zigbeed/RCP token migration](https://docs.silabs.com/zigbee/9.0.0/multiprotocol-solution-linux/host-ncp-rcp-migration),
requiring NVM3 and token-export support and explicitly warning about frame
counter rollback. This establishes a supported migration concept for Zigbeed,
not a ready-made importer for Ziggurat or proof that our installed NCP supports
the necessary export. Expect a service interruption even if pairings survive.
No radio flashing or production-service changes were performed for this audit.

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

`scripts/silabs-env.sh` selects `/usr/lib/jvm/java-17-openjdk-amd64/bin`.
Adjust that path on a different distribution. Rust and the MIPS RSDK are
needed for Ziggurat builds, but are not required for this ARM firmware build.
The installer supplies pinned **GSDK 4.5.0**, **SLC 5.6.3** and
**ARM GCC 12.2.rel1** under ignored `tools/silabs/`.

## Commands to build and verify firmware

Run from the experiment repository, **not** its parent `../`:

```sh
cd .
git submodule update --init --recursive
bash scripts/install-silabs.sh

# Ensure SDK binary libraries are downloaded, rather than LFS pointer files.
git -C tools/silabs/gecko_sdk lfs install --local
sdk_lfs_libraries='platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a'
sdk_lfs_libraries+=',protocol/openthread/libs/libsl_openthread_efr32mg1x_gcc.a'
sdk_lfs_libraries+=',platform/radio/rail_lib/autogen/librail_release/librail_efr32xg1_gcc_release.a'
git -C tools/silabs/gecko_sdk lfs pull --include="$sdk_lfs_libraries" --exclude=''

BUILD_JOBS=4 bash scripts/build-hdlc-firmware.sh
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
source scripts/silabs-env.sh
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
