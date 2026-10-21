# Build the gateway stack

Run these commands from the repository root on an x86_64 Linux development
computer. Building does not deploy software or access the gateway.
For device preparation, see [installation](installation.md).

## Prerequisites

On Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake git git-lfs curl ca-certificates \
  sshpass python3 python3-venv tar bzip2 xz-utils zstd ripgrep \
  openjdk-17-jre-headless picocom
```

The Web UI needs Node.js 22 and npm on the development computer. Radio builds
need at least 10 GiB for Silicon Labs tools, in addition to application build space.

```sh
git clone --recurse-submodules https://github.com/ChristophKaser/ziggurat-compilation.git embedded-home-automation
cd embedded-home-automation
git submodule sync --recursive
git submodule update --init --recursive
bash scripts/install-rust.sh
```

Component build scripts prepare patched sources in ignored worktrees. Edit
tracked source or patch series rather than treating those worktrees as the
source of record. Enable patch coverage checks with:

```sh
git config core.hooksPath .githooks
```

## Local development settings

Copy `workspace.local.env.example` to the gitignored `workspace.local.env` and
set your gateway address, LAN interface, compiler location and SSH credentials.
See [shared host settings](workspace-settings.md) for syntax and overrides.
Host tools read it automatically; no gateway is accessed by loading settings.

## Select a MIPS toolchain

Host builds need no MIPS SDK. Gateway builds default to RSDK
`rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129`, located beside this checkout.
Obtain the archive separately; the repository does not download it. Its SHA256 is
`63af001fcbebbf32e6235a66efb117d8abd13e088c8161abece7d578b55b56d2`.
If installed elsewhere, set `RSDK_ROOT` to its directory.

The source-built crosstool-NG compiler is selected explicitly:

```sh
python3 scripts/build-toolchain.py
export TOOLCHAIN=crosstool-ng
```

See [toolchain selection](../config/TOOLCHAINS.md) for compiler prerequisites,
cache isolation, overrides and runtime limits. RSDK remains the production
default. Radio firmware uses separate ARM tools regardless of `TOOLCHAIN`.
RSDK OpenThread builds additionally require Clang/LLVM 18.

## Build applications

Choose the MicroHAD backend that matches the installed radio firmware:

| Configuration | Command | RSDK output |
| --- | --- | --- |
| Ziggurat and Matter (default) | `make -C microhad mips` | `microhad/build/mips-ziggurat-matter/microhad` |
| Ziggurat without Matter | `make -C microhad ZIGGURAT=1 MATTER=0 mips` | `microhad/build/mips-ziggurat/microhad` |
| EZSP NCP | `make -C microhad ZIGGURAT=0 mips` | `microhad/build/mips/microhad` |

```sh
make -C microhad host test
make -C microhad webui
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash micro-thread-matter-daemon/scripts/build.sh mips service
bash firmware/scripts/build-maintenance.sh
```

| Artifact | Default output |
| --- | --- |
| Web UI | `microhad/webui/dist/` |
| Ziggurat multipan | `build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped` |
| Matter service | `build/micro-thread-matter-daemon/mthread-matter-mips` |
| Radio maintenance helper | `build/gateway-maintenance` |

Crosstool-NG outputs insert `crosstool-ng/` below the relevant `build/`
directory; Ziggurat also changes its target directory name. Use the exact
paths in the [toolchain artifact table](../config/TOOLCHAINS.md#artifacts-and-verification).

## Build and test on the host

After every MicroHAD C++ source/header change, run both `make -C microhad host test`
and `make -C microhad mips`. After a Web UI change, run `make -C microhad webui`.
See [test entrypoints](../microhad/test/TEST_PLAN.md) for focused integration checks.

Rebuild the synthetic CASE commissioning probe with the wrapper before native
Matter integration checks:

```sh
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
```

A plain Cargo build can write to a different target directory and leave the
harness using an old binary. [Matter component tests](../micro-thread-matter-daemon/README.md)
and [Ziggurat component tests](../ziggurat/README.md) use synthetic traffic without
accessing the gateway.

## Build radio firmware

```sh
bash firmware/scripts/install-silabs.sh
git -C firmware/dependencies/gecko-sdk lfs install --local
git -C firmware/dependencies/gecko-sdk lfs pull \
  --include='platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a,protocol/openthread/libs/libsl_openthread_efr32mg1x_gcc.a,platform/radio/rail_lib/autogen/librail_release/librail_efr32xg1_gcc_release.a' \
  --exclude=''
BUILD_JOBS=4 bash firmware/scripts/build-hdlc-firmware.sh
bash firmware/scripts/install-commander.sh
bash firmware/scripts/package-hdlc-gbl.sh
```

The installer selects GSDK 4.5.0, SLC 5.6.3 and ARM GCC 12.2.rel1. SLC uses an
ignored worktree. Java 17 defaults to the Debian/Ubuntu installation path;
other distributions may need an adjustment in `firmware/scripts/silabs-env.sh`.

Outputs are under `build/firmware-hdlc/build/debug/`. UART installation uses
`ot-rcp-multipan.gbl`, an application-only image. The raw `.bin` starts at
`0x4000` and is not a complete-chip image. Inspect each image before flashing;
a rebuilt image need not have the hash of an earlier build.

## Prepare installation and migration tools

For console access, password decoding, full-flash backup and the optional EZSP
TCP bridge, clone the companion tools beside this checkout:

```sh
git clone https://github.com/banksy-git/lidl-gateway-freedom.git ../lidl-gateway-freedom
python3 -m venv ../console-venv
../console-venv/bin/pip install pyserial cryptography
python3 -m venv tools/network-migration-venv
tools/network-migration-venv/bin/pip install \
  bellows==1.0.2 zigpy==2.2.0 universal-silabs-flasher==1.1.0
```

The serial bridge uses its RSDK build recipe:

```sh
. ./scripts/workspace-env.sh
workspace_load "$PWD"
make -C ../lidl-gateway-freedom/gateway CC="$RSDK_ROOT/bin/mips-linux-gcc" build
```

The upstream Makefile does not read our env file, so the command above loads
`RSDK_ROOT` first and passes its compiler explicitly. Avoid its upload target, which
contains a fixed device address. Transfer through SSH stdin as shown in
[installation](installation.md).

For the installation examples, define:

```sh
export PROJECT_ROOT="$PWD"
export WORKSPACE="$(dirname "$PROJECT_ROOT")"
export MIGRATION_PYTHON="$PROJECT_ROOT/tools/network-migration-venv/bin/python"
export NCP_IMAGE="$PROJECT_ROOT/firmware/dependencies/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/firmware/ncp-uart-hw-7.5.1-115200-hw.gbl"
export RCP_IMAGE="$PROJECT_ROOT/build/firmware-hdlc/build/debug/ot-rcp-multipan.gbl"
```

The pinned NCP 7.5.1 image uses EZSP v13, 115200 baud and hardware flow control;
its SHA256 is `05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d`.
The RCP runtime uses 460800 baud and hardware flow control. Both use the Gecko
bootloader at 115200 baud without flow control for uploads.
