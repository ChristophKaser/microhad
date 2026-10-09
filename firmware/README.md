# Radio firmware and maintenance

Rebuild and package the EFR32MG1B232 two-instance HDLC RCP firmware used by
Ziggurat and Thread/Matter. `dependencies/rtl8196e-gateway/` supplies the pinned
radio source/configuration and recovery images; `dependencies/slc-cli/` supplies
the generator. Gecko SDK is a firmware dependency under
`dependencies/gecko-sdk/`. These upstream pins remain unchanged.

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

Follow [installation](../docs/installation.md) and
[recovery](../docs/recovery-tooling.md) for authorized device work. Keep existing
network state and factory backups; shared-radio maintenance requires exclusive
UART ownership.
