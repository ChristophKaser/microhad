# Ziggurat gateway service

The gateway's Zigbee stack and radio multiplexer. The pinned upstream source is
in `dependencies/ziggurat/`; local patches and synthetic tooling live here.
`stock-uclibc-compat/` supplies syscall wrappers missing from factory uClibc
and an early illegal-instruction reporter. It is production compatibility code.

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

Gateway builds use RSDK by default. After `python3 scripts/build-toolchain.py`,
run `TOOLCHAIN=crosstool-ng ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh`
for the experimental compiler. Its multipan artifact is
`build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat.stripped`;
single-PAN output is under `build/crosstool-ng/ziggurat/`. See
[toolchain selection](../config/TOOLCHAINS.md) for source credits and limits.

Synthetic host tests use `build/host/debug/ziggurat`. Build that fixture with:

```sh
bash -c 'source scripts/rust-env.sh; CARGO_TARGET_DIR="$PWD/build/host" cargo +"$RUST_TOOLCHAIN" build --manifest-path worktrees/ziggurat-multipan/Cargo.toml -p ziggurat-server'
python3 ziggurat/scripts/test-simulate-rcp.py
python3 ziggurat/scripts/test-cobs-unix.py
python3 ziggurat/scripts/test-hdlc-multipan.py
python3 ziggurat/scripts/test-hdlc-multipan.py --thread-tcp
python3 ziggurat/scripts/test-fresh-join-replay.py
python3 ziggurat/scripts/test-end-device-relay.py
```

Run the multipan build first to prepare its worktree. Tests default to synthetic
host traffic; device options require a separately authorized hardware test.
See [building](../docs/building.md) and [integration](../docs/native-ziggurat-integration.md).

For a host OTBR, use `--thread-listen IP:PORT` instead of `--thread-socket`.
See [TCP Thread transport and no-reset configuration](../docs/hdlc-multipan-usage.md#tcp-endpoint-for-host-otbr).

## Licenses and attribution

Original project code is MIT licensed; upstream components retain their own terms.
See the repository-wide [third-party inventory](../THIRD_PARTY.md) for attribution,
notice generation and release requirements.
