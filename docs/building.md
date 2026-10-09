# Build the factory-Linux binaries and radio firmware

These commands run on an **x86_64 Linux development computer**, not the gateway.
Use Bash. Installation is covered in the [fresh-device guide](installation.md).
Allow at least 10 GiB for Silicon Labs tools plus additional Rust build space.

## Host and checkout layout

Ubuntu/Debian host packages:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake git git-lfs curl ca-certificates \
  sshpass python3 python3-venv tar bzip2 xz-utils zstd ripgrep \
  openjdk-17-jre-headless picocom
```

Native dashboard builds additionally need Node.js 22 with npm (the pinned Vite
6 package accepts Node 18/20/22+, but Node 22 is the documented build baseline). Do not install
Node.js on the gateway.

Choose a workspace directory; the example uses `$HOME/silvercrest-work`:

```sh
mkdir -p "$HOME/silvercrest-work"
cd "$HOME/silvercrest-work"
export WORKSPACE="$PWD"
git clone --recurse-submodules https://github.com/ChristophKaser/ziggurat-compilation.git embedded-home-automation
export PROJECT_ROOT="$WORKSPACE/embedded-home-automation"
git clone https://github.com/banksy-git/lidl-gateway-freedom.git
```

The seven submodules pin upstream Ziggurat, rs-matter, OpenThread,
rtl8196e-gateway, Gecko SDK, SLC CLI and TinyCBOR. Each component owns its dependencies.
Gecko SDK belongs to firmware; Thread/Matter owns its upstream OpenThread checkout.
TinyCBOR v0.6.0 is pinned under `microhad/dependencies/tinycbor`. The Matter
component is `micro-thread-matter-daemon/` and its executable is `mthread-matter`.
Both Matter daemons must use [CBOR IPC v2](../microhad/docs/matter-ipc-v2.md)
together; builds do not deploy the coordinated release.
MicroHAD is included in this checkout; no separate native repository is needed.

After updating an existing checkout, synchronize its moved submodules and
rebuild using the component entrypoints:

```sh
cd "$PROJECT_ROOT"
git submodule sync --recursive
git submodule update --init --recursive
```

## Choose the gateway toolchain

Host builds need no MIPS SDK. Gateway builds accept `TOOLCHAIN=rsdk` (the
production default) or `TOOLCHAIN=crosstool-ng` (experimental, built from source
without RSDK). Shared defaults live in `config/toolchains.mk`; Make, shell and
Python build entrypoints use the same selection.

For source-built gateway applications, run from the repository root:

```sh
cd "$PROJECT_ROOT"
python3 scripts/build-toolchain.py
export TOOLCHAIN=crosstool-ng
```

The compiler builder needs approximately 12 GiB free space, Python 3.11 or later,
GCC/G++, make, autoconf, automake, libtoolize, flex, bison, gperf, makeinfo,
rsync, patch, curl and tar. It installs the pinned Lexra/uClibc-ng compiler under
`tools/lexra-proof`; `CROSSTOOL_NG_ROOT` selects another compatible installation.
Application builds do not install the compiler automatically.

See [toolchain selection](../config/TOOLCHAINS.md) for prerequisites, isolated
artifacts, verification and experimental runtime limits. The configuration and
Lexra patches derive from `jnilo1/rtl8196e-gateway`; see
[configuration provenance and credits](../config/crosstool-ng/README.md).
The adapted uClibc-ng/LinuxThreads configuration differs from upstream's musl
profile. RSDK remains the production default; candidate application compilation
has been verified, but a complete candidate release has not been validated on
the gateway. Silicon Labs radio firmware uses its separate ARM tools.

### RSDK setup (default backend only)

Use exactly `rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129` beside the
`embedded-home-automation` checkout. Shared defaults in `config/toolchains.mk`
point gateway builds at this toolchain. Set `TOOLCHAIN=rsdk` and
`RSDK_ROOT="$RSDK"` when the toolchain is elsewhere.

The RSDK archive is **not included or downloaded by these repositories**.
Obtain the legacy archive from your existing toolchain distribution or the
maintainer before building. No verified public download URL is supplied here;
the newer upstream RSDK is not a tested substitute for these factory-Linux builds.
The archive used for this project has SHA256
`63af001fcbebbf32e6235a66efb117d8abd13e088c8161abece7d578b55b56d2`.

Host commands, after placing the archive in the workspace:

```sh
cd "$WORKSPACE"
echo '63af001fcbebbf32e6235a66efb117d8abd13e088c8161abece7d578b55b56d2  rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129.tar.bz2' | sha256sum --check
tar xjf rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129.tar.bz2
export RSDK="$WORKSPACE/rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129"
test -x "$RSDK/bin/mips-linux-g++"
export TOOLCHAIN=rsdk
export RSDK_ROOT="$RSDK"
```

If an old compiler cannot execute on your host, install the host compatibility
libraries it requires or use a compatible x86_64 Linux environment; do not
silently replace it with a modern compiler.

### What “Lexra compiler” means here

Gateway applications run on the same big-endian Lexra CPU. Rust's LLVM backend
emits MIPS-I/soft-float code; the selected GCC links it against that toolchain's
uClibc runtime/sysroot. The selected GCC/G++ compiles native C/C++.
OpenThread uses Clang 18 with the RSDK linker and headers for `TOOLCHAIN=rsdk`,
and native GCC/G++ for `TOOLCHAIN=crosstool-ng`.

## Native service and serial bridge

Host commands:

```sh
cd "$PROJECT_ROOT/microhad"
make ZIGGURAT=0 host test
make ZIGGURAT=0 mips
make ZIGGURAT=1 MATTER=0 host test
make ZIGGURAT=1 MATTER=0 mips
make mips                  # default Ziggurat + Matter
make webui
```

The optional external serial bridge uses its legacy RSDK recipe (its build
has not been validated with the candidate compiler):

```sh
cd "$WORKSPACE/lidl-gateway-freedom/gateway"
make CC="$RSDK/bin/mips-linux-gcc" build
```

The MicroHAD commands use the selected `TOOLCHAIN`; its candidate C/C++ builds
default to `-Os`. Only build the backend you intend to install. RSDK outputs
remain under `microhad/build/mips*`; candidate outputs are under
`microhad/build/crosstool-ng/mips*`.
Bridge output is `lidl-gateway-freedom/gateway/serialgateway`. The bridge's
`make upload` target contains an old device address: do not use it.

## MIPS Ziggurat and maintenance helper

Host commands:

```sh
cd "$PROJECT_ROOT"
bash scripts/install-rust.sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash firmware/scripts/build-maintenance.sh
```

Rust is pinned to `nightly-2026-10-02`, with local Cargo/rustup homes. The build
applies this repository's transport, COBS, migration, stock-Linux and subsequent
Zigbee fixes in an ignored worktree. With the default RSDK selection it produces:

- `build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped`
- `build/gateway-maintenance`

With `TOOLCHAIN=crosstool-ng`, these become
`build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat.stripped`
and `build/crosstool-ng/gateway-maintenance`. Host fixture paths stay unchanged.

Enable the repository's pre-commit patch coverage check once per clone:

```sh
git config core.hooksPath .githooks
```

The hook validates the stock and multipan Ziggurat series, both rs-matter
series, OpenThread, and the three Cargo compatibility patches. When their
build worktrees exist, it also checks that their tracked changes match the
patch series.

Use the multi-PAN build for IID 1, even if Thread is disabled. The helper provides
UART ownership checks and bootloader/COBS maintenance, not a daily extra daemon.

## OpenThread MIPS proof build

OpenThread uses Clang 18 C/C++ with the RSDK linker and uClibc libraries for
`TOOLCHAIN=rsdk`, or the candidate GCC/G++ and sysroot for
`TOOLCHAIN=crosstool-ng`. Only the RSDK path needs the Clang/LLVM 18 tools.
It initializes upstream OpenThread v2026.10.0 and its pinned
Mbed TLS dependency, then applies the Unix HDLC adapter in an ignored build worktree. It does not
require the firmware SDK installer or deploy to the gateway.

```sh
cd "$PROJECT_ROOT"
bash micro-thread-matter-daemon/scripts/build-openthread.sh host
bash micro-thread-matter-daemon/scripts/build-openthread.sh mips
```

The host command builds the CLI and runs the real HDLC adapter against a local
Unix socket. The MIPS command creates `build/openthread-unix/ot-cli-mips` and
checks ELF architecture, soft-float attributes and static linking. Candidate
output is `build/crosstool-ng/openthread-unix/ot-cli-mips`. Both toolchain paths
were compiled locally on 2026-10-09; earlier device proof results used RSDK.
In restricted execution environments, host Unix
socket tests and the legacy RSDK linker may require execution outside the sandbox.

The CLI uses OpenThread's internal stack and does not require Linux TUN.
The running factory kernel lacks IPv6/TUN for the Linux interface/border-router
path. The binary passed a bounded device simulation using a host-simulated
RCP and internal UDP self delivery; it is not installed as a service.
See [compiler fixes, relay build/test commands, artifact hash and limits](openthread-feasibility.md)
for the complete evidence and procedure. OTBR is outside this project; the separate Matter service uses this userspace stack.

## Two-instance HDLC RCP firmware

Host commands:

```sh
cd "$PROJECT_ROOT"
bash firmware/scripts/install-silabs.sh
git -C firmware/dependencies/gecko-sdk lfs install --local
sdk_lfs_libraries='platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a'
sdk_lfs_libraries+=',protocol/openthread/libs/libsl_openthread_efr32mg1x_gcc.a'
sdk_lfs_libraries+=',platform/radio/rail_lib/autogen/librail_release/librail_efr32xg1_gcc_release.a'
git -C firmware/dependencies/gecko-sdk lfs pull --include="$sdk_lfs_libraries" --exclude=''
BUILD_JOBS=4 bash firmware/scripts/build-hdlc-firmware.sh
bash firmware/scripts/install-commander.sh
bash firmware/scripts/package-hdlc-gbl.sh
```

The installer pins GSDK 4.5.0, SLC 5.6.3 and ARM GCC 12.2.rel1. SLC runs from
the ignored linked worktree `build/slc-cli`, keeping its generated state out of
the pinned `firmware/dependencies/slc-cli` submodule. Java 17 is selected
at `/usr/lib/jvm/java-17-openjdk-amd64/bin`; adjust `firmware/scripts/silabs-env.sh` for
another distribution. SDK binary libraries must be downloaded, not LFS pointers.

Outputs are under `build/firmware-hdlc/build/debug/`: `.out`, `.bin`, `.map` and
`ot-rcp-multipan.gbl`. Use the **application-only GBL** for UART installation;
the raw binary's base is `0x4000`, and it is not a complete-chip image.
Packaging/build commands do not contact or flash a device.

## Host radio tools and pinned NCP image

Host commands:

```sh
cd "$PROJECT_ROOT"
python3 -m venv tools/network-migration-venv
tools/network-migration-venv/bin/pip install \
  bellows==1.0.2 zigpy==2.2.0 universal-silabs-flasher==1.1.0
export MIGRATION_PYTHON="$PROJECT_ROOT/tools/network-migration-venv/bin/python"
export NCP_IMAGE="$PROJECT_ROOT/firmware/dependencies/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/firmware/ncp-uart-hw-7.5.1-115200-hw.gbl"
export RCP_IMAGE="$PROJECT_ROOT/build/firmware-hdlc/build/debug/ot-rcp-multipan.gbl"
cd "$PROJECT_ROOT/microhad"
"$MIGRATION_PYTHON" scripts/ezsp_preflight.py image "$NCP_IMAGE"
# After the HDLC build:
"$MIGRATION_PYTHON" scripts/ezsp_preflight.py image "$RCP_IMAGE"
```

Known artifact pins:

| Image | SHA256 | Purpose |
| --- | --- | --- |
| NCP 7.5.1 / EZSP v13 / 115200 HW | `05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d` | Native EZSP baseline and NCP rollback |
| Two-instance HDLC RCP | `5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48` | Ziggurat IID 1, reserved future Thread IID 2 |

The inspector checks whole-image CRC and rejects bootloader/secure-element,
compressed/uninspectable or out-of-range application updates. If a rebuilt
image differs from its recorded pin, inspect and explain that difference before
updating the deployment record; do not blindly substitute its hash to bypass a
mismatch. Never use the upstream replacement-Linux `flash_efr32.sh` unchanged on
factory Linux: it expects different bridge and `/userdata` interfaces.

## Rust Matter service

Build `mthread-matter` with the component scripts in
`micro-thread-matter-daemon/`. See the [component README](../micro-thread-matter-daemon/README.md)
for host/MIPS builds, pinned dependencies and synthetic tests, and the
[integration guide](../microhad/MATTER_INTEGRATION.md) for the paired runtime.
Native and Rust daemons must use [CBOR IPC v2](../microhad/docs/matter-ipc-v2.md)
together. `TOOLCHAIN=crosstool-ng` selects the candidate MIPS build and writes
`build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips`; host fixtures
keep their existing paths. Builds do not deploy a release.
