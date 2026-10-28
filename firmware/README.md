# Radio firmware and maintenance

Rebuild and package the EFR32MG1B232 two-instance HDLC RCP firmware used by
Ziggurat and Thread/Matter. `dependencies/rtl8196e-gateway/` supplies the pinned
radio source/configuration and recovery images; `dependencies/slc-cli/` supplies
the generator. Gecko SDK is a firmware dependency under
`dependencies/gecko-sdk/`. These upstream pins remain unchanged.

Enabling firmware radio tracing causes unstable radio behavior on this
shared-radio gateway, including failed Matter CASE sessions and RCP timeouts.
It is only for debugging a specific problem during a short, explicitly
requested investigation. Keep it disabled for normal operation and routine
software packet capture, and disable it immediately after that investigation.

See [firmware tracing controls](../docs/recovery-tooling.md#check-readiness-and-logs).

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

## Prebuilt firmware releases

Download `ot-rcp-multipan-VERSION.tar.gz` from the repository's
[GitHub releases](https://github.com/ChristophKaser/ziggurat-compilation/releases).
Extract it on a Linux host and follow its `readme.md`:

```sh
tar xzf ot-rcp-multipan-VERSION.tar.gz
cd ot-rcp-multipan-VERSION
python3 -m venv .venv
.venv/bin/pip install bellows==1.0.2 universal-silabs-flasher==1.1.0
.venv/bin/python flash.py
```

This validates the GBL offline. The archive also contains `license.md` with
radio-specific SDK, source-header and linked-runtime notices. It requires no
SDK, compiler or repository checkout to use. The package does not install
Ziggurat or migrate a Zigbee network.

For an HDLC update, first [quiesce and save a frozen snapshot](../docs/recovery-tooling.md#orderly-stop-and-frozen-zigbee-export).
Then run from the extracted package:

```sh
.venv/bin/python flash.py --host 192.168.1.50 --radio hdlc \
  --frozen-snapshot-saved --flash
```

**WARNING: REPLACING EZSP MAY REQUIRE RECREATING YOUR ZIGBEE NETWORK.**
To preserve an existing network, follow [Ziggurat migration](../docs/ziggurat-deployment-plan.md)
through final capture and bootloader entry before using `--radio ezsp
--already-in-bootloader --acknowledge-ezsp-replacement --flash`. Stop the
capture's bridge first. Direct replacement of an EZSP radio is supported
without `--already-in-bootloader`, with the same explicit acknowledgment.
EZSP services remain stopped after replacement; do not reboot into their old
launcher. Factory services and external coordinators must be stopped separately.

The host uses SSH to prepare the gateway and uploads through a temporary TCP
bridge. If a compatible `/tuya/serialgateway` is absent, it downloads the pinned
[upstream Release-1.2 binary](https://github.com/banksy-git/lidl-gateway-freedom/releases/tag/Release-1.2),
verifies SHA-256 and stages it under `/tmp` over SSH stdin. The gateway needs
neither Internet access nor SCP. Use `--serialgateway PATH` for an offline
copy of that binary. Existing installed files are not overwritten.
Use `--password-file /private/gateway-password` with host `sshpass` for unattended
SSH password authentication, or `--identity PATH` for key authentication. Stock
BusyBox tool limitations and LinuxThreads worker PIDs are handled by the flasher.

## Build a firmware release

After installing the source-build tools above, run from the repository root
using a Python environment with the flashing dependencies:

```sh
tools/network-migration-venv/bin/python firmware/scripts/build-release.py \
  --version VERSION --jobs 4
tools/network-migration-venv/bin/python firmware/scripts/test-release.py
```

If that virtualenv does not exist, create a Python 3.11+ virtualenv and install
the pinned dependencies shown above, then use its Python executable instead.
The builder rebuilds firmware, packages and validates the application-only GBL,
and writes `build/releases/firmware/VERSION/ot-rcp-multipan-VERSION.tar.gz`.
Existing release output is refused. No device is contacted and nothing is
published. Commit release inputs before publishing; local changes are identified
in the packaged README. SDK/reference dependencies must be unmodified.
`SOURCE_DATE_EPOCH` controls archive timestamps; otherwise the project commit
timestamp is used. Missing notices or unreviewed link inputs abort packaging.

## Licenses and attribution

Original project code is MIT licensed; upstream components retain their own terms.
See the repository-wide [third-party inventory](../THIRD_PARTY.md) for attribution,
notice generation and release requirements.
The release builder records applicable SDK/per-file terms and runtime
license supplements, with the components covered by each text.
