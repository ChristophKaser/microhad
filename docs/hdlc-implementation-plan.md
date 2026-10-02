# HDLC multi-PAN implementation plan

Started 2026-10-02. The goal is one Tuya-owned radio transport for Zigbee IID 1
and Thread IID 2, without cpcd. The eventual Zigbee client is
`microhad-zigbee-native`; Thread and Matter must also run on the Tuya.

## Work and acceptance criteria

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

## Progress

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
