# Radio firmware, HDLC and standalone dual mode

Source audit, 2026-10-02. No firmware was compiled or flashed in this audit;
the physical UART and production gateway services were not touched.

Subsequent firmware builds and transport implementation are tracked in
[the implementation plan](hdlc-implementation-plan.md) and
[usage/results](hdlc-multipan-usage.md); statements below describe this initial audit.

## Outcome

CPC is a transport choice, not a requirement for sending Zigbee and Thread
through the same radio. GSDK 4.5.0 contains multi-instance HDLC initialization,
multiple address filters and radio-side source matching. The promising route
is to enable those existing facilities in the gateway's HDLC RCP image and
share its UART inside a gateway process. This is a source-supported proposal,
not a working firmware/host combination yet.

The fixed product goal is a standalone Tuya gateway: `microhad-zigbee-native` as
Ziggurat's Zigbee client, local Thread border-router functionality and local
Matter application functionality. An external computer is only for development.
Avoiding cpcd saves one service, but does not remove the Thread stack, IP border
routing, commissioning or Matter application responsibilities. Integrating
components into one process still requires measuring their combined resource use.

## What was inspected

- Gateway repository: `30eb5e5b9ee334d2c6d76812acd4a6cdf2923423`.
- Ziggurat: `392d4c7fe5c497d553977492bc02cb305573b7fc`.
- Gecko SDK: tag `v4.5.0`, commit
  `90fc93e1f95a10c981f0e49ca6787192e82fd8f2`, matching the gateway installer.
- Selected SDK files were downloaded into ignored `build/radio-audit/`.
  [The manifest](radio-source-manifest.json) records their paths and SHA-256
  hashes. It excludes transient GitHub responses and the large tree listing.

This audit concerns the proposed OpenThread RCP, not the source or licensing
of the original Tuya coordinator firmware. That original firmware was not audited.

## Why arbitrary frames alone are insufficient

Both networks use IEEE 802.15.4, so one transmitter can serialize their frames.
Receiving requires the correct destination PAN/address filters and immediate
ACK behavior. Sleepy children also depend on the ACK Frame Pending bit to learn
whether their parent has queued data. Those decisions must happen at the radio.
The SDK configures RX-to-TX timing at 192 microseconds; our host simulator's
millisecond timings cannot validate this requirement. Raw transmission is
therefore necessary but does not establish reliable dual-network reception.

Promiscuous mode removes filtering. It is not evidence that both networks get
correct ACKs or Frame Pending bits. Two clients repeatedly overwriting one
PAN/address configuration would also be insufficient. Network contexts can be
implemented without CPC; the existing SDK multi-PAN machinery does exactly that.

## Hardware and driver findings

The [RAIL IEEE 802.15.4 header][rail-header] defines three address slots, each
with a PAN ID, short address and extended address. Matching pairs enable
reception for multiple PANs. The [multi-PAN helper][multipan] maps IIDs 1–3 to
these slots; IID 0 is reserved for broadcast in multi-PAN mode.

The [EFR32 driver][radio] programs addresses per instance and enables auto-ACK.
It keeps instance-tagged transmit/scan requests and dispatches completion and
receive events. Broadcast reception is delivered to initialized instances.
Its ACK path consults source-match tables while still on the radio.

The [EFR32 source-match implementation][source-match] has per-PAN tables.
Its introductory single-instance comment is stale relative to the code;
the arrays and IID-aware functions explicitly support multi-PAN.

MG1 has a significant limitation: the driver says its RAIL filter mask does not
work. For immediate ACK source matching with an ambiguous IID it searches all
network tables; the enhanced-ACK path also has an MG1-specific PAN lookup and
fallback. An overlapping short address can consequently cause a Frame Pending
decision based on another network's table. This is a test requirement, not
proof of a fatal failure: check both networks' sleepy children, overlapping
short addresses, PAN broadcasts and extended addressing.

Some driver controls remain global, including promiscuous mode and the
source-match enable flag. Multi-PAN does not mean every radio setting is
independent. Ziggurat normally enables source matching; either client changing
global settings needs an explicit policy.

For this Series-1 gateway, use a common Zigbee/Thread channel as the baseline.
The gateway project's existing multi-PAN demonstration uses the same channel.
Generic SDK fast channel-switching code does not prove that this MG1 supports
reliable reception on two channels. Transmissions and scans share radio time;
scan interference must be measured even with one common operating channel.

## HDLC already has the required multi-instance entry points

The SDK's [NCP HDLC implementation][hdlc] provides `otNcpHdlcInitMulti` and a
multi-instance `NcpHdlc` constructor under the multi-PAN/RCP guards.
The [upstream NCP application glue][ncp-app] provides `otAppNcpInitMulti`,
enables UART and calls that initializer. The [SDK sample app][app] creates
static instances and selects the multi-instance initializer when configured.

There is an initialization caveat: [Silicon Labs' non-CPC vendor-hook glue][init]
only defines the single-instance initialization path. The generated project
must either select the ordinary upstream multi-instance glue or extend the
vendor-hook path. Verify generated sources and symbols; changing defines alone
is not yet a demonstrated working build.

In the pinned gateway project:

- `26-OT-RCP/patches/ot-rcp.slcp` selects the ordinary UART RCP, without the
  multiple-instance settings. It is currently a single-network image.
- `26-OT-RCP/build_ot_rcp.sh` copies `app.c` from the SDK sample directory;
  it does not use the repository's tracked `patches/app.c`. Inspecting that
  patch alone would give the wrong picture of the built initialization.
- `25-RCP-UART-HW/patches/rcp-uart-802154.slcp` enables multi-PAN, two static
  instances, broadcast IID 0 and CPC transport. Its multi-instance settings
  are useful references, but its transport components must not be copied
  wholesale into the HDLC experiment.

A candidate HDLC build should keep UARTDRV/hardware flow control and enable
`OPENTHREAD_CONFIG_MULTIPAN_RCP_ENABLE`, `MULTIPLE_INSTANCE_ENABLE`,
`MULTIPLE_STATIC_INSTANCE_ENABLE`, and `MULTIPLE_INSTANCE_NUM=2` (all with the
`OPENTHREAD_CONFIG_` prefix), while selecting working HDLC multi-instance glue.
Check broadcast IID configuration, generated radio parameters and final map.
Keep the Lidl board wiring and baud rate from the current build recipe.

## Required host changes

Ziggurat parses the Spinel IID, but its request/reset construction currently
uses IID 0. Its pending-response map is keyed by transaction ID alone.
See `ziggurat/crates/ziggurat-spinel/src/{client.rs,lib.rs}` and
`ziggurat/crates/ziggurat-phy-spinel/src/lib.rs`.

Use one physical UART owner, with Zigbee on IID 1 and Thread on IID 2.
An in-process dispatcher can route incoming Spinel messages and serialize
outgoing HDLC frames without adding cpcd. Keep transaction tracking separate
per IID, reject cross-interface responses, and distribute global reset/status
events intentionally. Define one reset owner; a Thread startup/reset must not
silently destroy the Zigbee radio state. Broadcast handling must avoid duplicate
delivery and still allow the stacks to reject unrelated network frames.

Thread may be embedded through a platform interface or run in a local
`otbr-agent` with an adapted transport. Two processes opening the UART directly
is not a solution. A local socket transport would require appropriate OTBR
integration. A PTY is another possibility only after resolving the stock
device's unavailable devpts setup; the earlier PTY probe failed with ENXIO.
Process count alone does not establish memory savings; measure each design.

## Is the firmware open source?

**Partly. The SDK-based image is rebuildable using published source plus binary
libraries, but it is not entirely open source.**

| Component | Inspected licensing/source status |
| --- | --- |
| Gateway project's own scripts/patches | Repository MIT license; does not relicense SDK dependencies |
| OpenThread core and inspected NCP/HDLC code | BSD 3-clause headers, source published |
| Inspected EFR32 radio driver, multi-PAN helper and source-match implementation | BSD 3-clause headers, source published and editable |
| SDK sample `app.c` | Published source under Silicon Labs MSLA |
| RAIL IEEE 802.15.4 header | Published API header under Zlib license |
| MG1 RAIL radio implementation | Precompiled `.a` library dependency; implementation source not supplied by the inspected SDK component |

The [single-protocol RAIL component][rail-component] selects
`librail_efr32xg1_gcc_release.a` for the relevant GCC device configuration.
The [SDK license file][license] explicitly licenses libraries under
`platform/radio/rail_lib/autogen/librail_release` using Zlib; other SDK files
default to MSLA unless individually marked otherwise. Permissive distribution
terms for an archive do not provide its implementation source. Calling the
whole firmware “open source” would conceal this binary dependency and mixed
licensing. Replacing every radio-layer component would be a separate project.

## Next gates and validation limits

1. Generate and compile a disposable two-instance HDLC project; inspect
   initialization symbols and Flash/RAM map against the MG1 device and
   bootloader layout. Neither `slc` nor `arm-none-eabi-gcc` was on PATH during
   this audit; no full firmware build was attempted.
2. Extend the simulator to two IIDs and exercise isolation, resets, broadcasts
   and transactions before modifying production radio ownership.
3. Implement the shared host transport and benchmark the local Thread stack
   together with Ziggurat and `microhad-zigbee-native` on the Tuya.
4. Hardware validation requires a backed-up, explicitly authorized firmware
   test: both PANs' unicast ACKs, sleepy-child polls, address collisions,
   commissioning/scans, sustained traffic and recovery.
5. Add and measure the required Matter application. Earlier Zigbee-only
   simulation and CPC daemon startup results do not demonstrate standalone
   Thread/Matter feasibility or radio interoperability.

The source audit removes “CPC is mandatory” as a blocker. It does not remove
the radio/firmware build, host integration, resource and hardware validation gates.

[rail-header]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/platform/radio/rail_lib/protocol/ieee802154/rail_ieee802154.h
[multipan]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/protocol/openthread/platform-abstraction/efr32/sl_multipan.h
[radio]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/protocol/openthread/platform-abstraction/efr32/radio.c
[source-match]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/protocol/openthread/platform-abstraction/efr32/soft_source_match_table.c
[hdlc]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/util/third_party/openthread/src/ncp/ncp_hdlc.cpp
[ncp-app]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/util/third_party/openthread/examples/apps/ncp/ncp.c
[app]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/protocol/openthread/sample-apps/ot-ncp/app.c
[init]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/protocol/openthread/platform-abstraction/ncp/ncp_init.cpp
[rail-component]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/platform/radio/rail_lib/component/rail_lib_singleprotocol.slcc
[license]: https://github.com/SiliconLabs/gecko_sdk/blob/90fc93e1f95a10c981f0e49ca6787192e82fd8f2/License.txt
