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
