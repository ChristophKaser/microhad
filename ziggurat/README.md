# Ziggurat gateway service

The gateway's Zigbee stack and radio multiplexer. The pinned upstream source is
in `dependencies/ziggurat/`; local patches and synthetic tooling live here.
`stock-uclibc-compat/` supplies syscall wrappers missing from factory uClibc
and early SIGILL/SIGSEGV register and fault-address capture. It is production compatibility code.

Host tools read compiler, gateway and SSH settings from the private root
`workspace.local.env`; see [shared host settings](../docs/workspace-settings.md).
Explicit environment and command-line overrides still apply.

From the repository root:

```sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
ZIGGURAT_MULTIPAN=0 bash ziggurat/scripts/build.sh
```

The default multipan build writes
`build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped`.
The alternate stock transport build writes under `build/ziggurat/`.
Both apply their patch series to ignored worktrees, preserving upstream source.
Gateway builds override the upstream release optimization level with `opt-level="s"`
to reduce binary size, retaining upstream LTO, stripping and abort-on-panic settings.
They disable the optional `websocket` feature; MicroHAD uses `--api cobs-unix`.
Without WebSockets the API defaults to stdio. Thread HDLC Unix/TCP endpoints
remain available. Logging supports global and module levels in `RUST_LOG`
(for example, `info,ziggurat_driver=debug`), but no span/field regex filters.

Gateway builds use RSDK by default. After `python3 scripts/build-toolchain.py`,
run `TOOLCHAIN=crosstool-ng ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh`
for the source-built compiler. Its multipan artifact is
`build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat.stripped`;
single-PAN output is under `build/crosstool-ng/ziggurat/`. See
[toolchain selection](../config/TOOLCHAINS.md) for source credits and limits.

Synthetic host tests use `build/host/debug/ziggurat`. Build that fixture with:

```sh
bash -c 'source scripts/rust-env.sh; CARGO_TARGET_DIR="$PWD/build/host" cargo +"$RUST_TOOLCHAIN" build --manifest-path worktrees/ziggurat-multipan/Cargo.toml -p ziggurat-server --no-default-features'
python3 ziggurat/scripts/test-simulate-rcp.py
python3 ziggurat/scripts/test-cobs-unix.py
python3 ziggurat/scripts/test-hdlc-multipan.py --cobs-unix
python3 ziggurat/scripts/test-hdlc-multipan.py --cobs-unix --thread-tcp
python3 ziggurat/scripts/test-fresh-join-replay.py
python3 ziggurat/scripts/test-end-device-relay.py
python3 ziggurat/scripts/test-neighbor-recovery.py
```

Run the multipan build first to prepare its worktree. Tests default to synthetic
host traffic; device options require a separately authorized hardware test.
See [building](../docs/building.md) and [integration](../docs/native-ziggurat-integration.md).

For a host OTBR, use `--thread-listen IP:PORT` instead of `--thread-socket`.
See [TCP Thread transport and no-reset configuration](../docs/hdlc-multipan-usage.md#tcp-endpoint-for-host-otbr).

## Fresh join recovery

Join notifications keep the 13-byte payload. Bit 3 (`0x08`) of the final flags
byte now marks an accepted fresh association or router-reported unsecured new
join. Secured and trust-center rejoins leave it clear; ordinary Device Announce
packets do not imply configuration loss. Existing child capability bits retain
their meaning. Older Ziggurat builds leave the new bit clear.

MicroHAD uses this evidence to invalidate the device's saved configuration
signature, restart endpoint discovery and reporting/binding setup, and then
restore its configured groups, scenes and native remote bindings. Restoration
uses the existing acknowledged provisioning queues and only replaces work for
the affected device. Existing timeout/retry behavior applies. Ordinary
announcements and preserving rejoins keep their cached configuration.

Update both Ziggurat and MicroHAD to enable automatic fresh-join recovery. This
change does not retroactively repair a reset device whose fresh join happened
before the update, and does not change the network keys or identity.

## Licenses and attribution

Original project code is MIT licensed; upstream components retain their own terms.
See the repository-wide [third-party inventory](../THIRD_PARTY.md) for attribution,
notice generation and release requirements.

## Neighbor recovery

A complete empty Link Status advertisement requests a populated, encrypted
one-hop response after 1–2 seconds of jitter. Requests share one pending slot;
extra responses are limited to one per link-status period (15 seconds by
default). Periodic advertisements continue independently, and extra responses
do not accelerate neighbor or route activity ageing.

When a neighbor becomes bidirectional again, the stack wakes pending routing
work and the concentrator advertisement scheduler, preserving its minimum
spacing. Each queued discovery may send one additional route request, without
extending its original deadline or replaying commands already failed. This
improves recovery from lost links; it does not diagnose the cause of their loss.

The synthetic neighbor recovery fixture uses only a loopback radio and test
keys; it checks authentication, bootstrap coalescing, queued delivery, bounded
retries and timely terminal failure.
