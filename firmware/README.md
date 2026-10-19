# Radio firmware and maintenance

Rebuild and package the EFR32MG1B232 two-instance HDLC RCP firmware used by
Ziggurat and Thread/Matter. `dependencies/rtl8196e-gateway/` supplies the pinned
radio source/configuration and recovery images; `dependencies/slc-cli/` supplies
the generator. Gecko SDK is a firmware dependency under
`dependencies/gecko-sdk/`. These upstream pins remain unchanged.

Host tools read compiler, gateway and SSH settings from the private root
`workspace.local.env`; see [shared host settings](../docs/workspace-settings.md).
Explicit environment and command-line overrides still apply.

From the repository root:

```sh
bash firmware/scripts/install-silabs.sh
bash firmware/scripts/install-commander.sh
bash firmware/scripts/build-hdlc-firmware.sh
bash firmware/scripts/package-hdlc-gbl.sh
bash firmware/scripts/build-maintenance.sh
python3 firmware/scripts/test-maintenance.py
```

Build and packaging commands do not access a gateway or flash a radio. The GBL
is written under `build/firmware-hdlc/build/debug/`; the maintenance helper is
`build/gateway-maintenance`, with a host test binary alongside it.

Radio firmware uses the pinned ARM toolchain, independently of `TOOLCHAIN`.
The MIPS maintenance helper uses RSDK by default. After building the candidate
with `python3 scripts/build-toolchain.py`, run
`TOOLCHAIN=crosstool-ng bash firmware/scripts/build-maintenance.sh` to produce
`build/crosstool-ng/gateway-maintenance`. See
[toolchain selection](../config/TOOLCHAINS.md) for prerequisites and limits.

Follow [installation](../docs/installation.md) and
[recovery](../docs/recovery-tooling.md) for authorized device work. Keep existing
network state and factory backups; shared-radio maintenance requires exclusive
UART ownership.

## Licenses and attribution

Original project code is MIT licensed; upstream components retain their own terms.
See the repository-wide [third-party inventory](../THIRD_PARTY.md) for attribution,
notice generation and release requirements.
