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
git clone --recurse-submodules https://github.com/ChristophKaser/ziggurat-compilation.git
git clone https://github.com/ChristophKaser/microhad-zigbee-native.git
git clone https://github.com/banksy-git/lidl-gateway-freedom.git
```

Access to the private repositories is required. The experiment's submodules
pin Ziggurat, rtl8196e-gateway and cpc-daemon; **cpc-daemon is not built or run**
for these HDLC modes. Use the standalone native repository checkout, rather than
applying its archived patch over an unrelated revision.

## Stock MIPS toolchain

Use exactly `rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129` beside the two project
checkouts. Ziggurat's linker and maintenance build derive this sibling location.
The native Makefile has a developer-machine default; override it with
`RSDK_ROOT="$RSDK"` on every target build.

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
```

If an old compiler cannot execute on your host, install the host compatibility
libraries it requires or use a compatible x86_64 Linux environment; do not
silently replace it with a modern compiler.

### What “Lexra compiler” means here

Both applications run on the same big-endian Lexra CPU. Their current build
paths differ: Rust's LLVM backend generates Ziggurat code using our custom
MIPS-I/soft-float target, while the stock RSDK compiles native C/C++. Ziggurat
still uses that RSDK's GCC driver as linker and its uClibc runtime/sysroot; see
[`make-llvm-target.sh`](../scripts/make-llvm-target.sh). Rust is not a C++ compiler.

Using Clang/LLVM for native C++ may be possible, but needs a separate verified
build configuration with compatible C++ headers/libraries, ABI, atomics and
Lexra instruction generation. That path has not been implemented or tested.
The newer rtl8196e-gateway toolchain targets another Linux environment and is
also not a verified drop-in replacement. The commands here reproduce the
existing tested builds; they do not imply RSDK-free compilation of Ziggurat.

## Native service and serial bridge

Host commands:

```sh
cd "$WORKSPACE/microhad-zigbee-native"
make RSDK_ROOT="$RSDK" ZIGGURAT=0 host test
make RSDK_ROOT="$RSDK" ZIGGURAT=0 mips
make RSDK_ROOT="$RSDK" ZIGGURAT=1 host test
make RSDK_ROOT="$RSDK" ZIGGURAT=1 mips
make webui

cd "$WORKSPACE/lidl-gateway-freedom/gateway"
make CC="$RSDK/bin/mips-linux-gcc" build
```

Only build the backend you intend to install; both are shown for contributors.
Bridge output is `lidl-gateway-freedom/gateway/serialgateway`. The bridge's
`make upload` target contains an old device address: do not use it.

## MIPS Ziggurat and maintenance helper

Host commands:

```sh
cd "$WORKSPACE/ziggurat-compilation"
bash scripts/install-rust.sh
ZIGGURAT_MULTIPAN=1 bash scripts/build-ziggurat.sh
bash scripts/build-maintenance.sh
```

Rust is pinned to `nightly-2026-10-02`, with local Cargo/rustup homes. The build
applies this repository's transport, COBS, migration, stock-Linux and subsequent
Zigbee fixes in an ignored worktree. It produces:

- `build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped`
- `build/gateway-maintenance`

Use the multi-PAN build for IID 1, even if Thread is disabled. The helper provides
UART ownership checks and bootloader/COBS maintenance, not a daily extra daemon.

## OpenThread MIPS proof build

OpenThread uses a separate Clang 18 C/C++ build with the stock RSDK linker and
uClibc libraries. It reuses the installed GSDK source and applies the Unix HDLC
adapter in an ignored copy; it does not alter the SDK or deploy to the gateway.

```sh
cd "$WORKSPACE/ziggurat-compilation"
bash scripts/build-openthread.sh host
bash scripts/build-openthread.sh mips
```

The host command builds the CLI and runs the real HDLC adapter against a local
Unix socket. The MIPS command creates `build/openthread-unix/ot-cli-mips` and
checks ELF architecture, soft-float attributes and static linking. Both commands
were verified on 2026-10-03. In restricted execution environments, host Unix
socket tests and the legacy RSDK linker may require execution outside the sandbox.

The CLI uses OpenThread's internal stack and does not require Linux TUN.
The running factory kernel lacks IPv6/TUN for the Linux interface/border-router
path. The binary passed a bounded device simulation using a host-simulated
RCP and internal UDP self delivery; it is not installed as a service.
See [compiler fixes, relay build/test commands, artifact hash and limits](openthread-feasibility.md)
for the complete evidence and procedure. OTBR and Matter are still separate work.

## Two-instance HDLC RCP firmware

Host commands:

```sh
cd "$WORKSPACE/ziggurat-compilation"
bash scripts/install-silabs.sh
git -C tools/silabs/gecko_sdk lfs install --local
sdk_lfs_libraries='platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a'
sdk_lfs_libraries+=',protocol/openthread/libs/libsl_openthread_efr32mg1x_gcc.a'
sdk_lfs_libraries+=',platform/radio/rail_lib/autogen/librail_release/librail_efr32xg1_gcc_release.a'
git -C tools/silabs/gecko_sdk lfs pull --include="$sdk_lfs_libraries" --exclude=''
BUILD_JOBS=4 bash scripts/build-hdlc-firmware.sh
bash scripts/install-commander.sh
bash scripts/package-hdlc-gbl.sh
```

The installer pins GSDK 4.5.0, SLC 5.6.3 and ARM GCC 12.2.rel1. Java 17 is selected
at `/usr/lib/jvm/java-17-openjdk-amd64/bin`; adjust `scripts/silabs-env.sh` for
another distribution. SDK binary libraries must be downloaded, not LFS pointers.

Outputs are under `build/firmware-hdlc/build/debug/`: `.out`, `.bin`, `.map` and
`ot-rcp-multipan.gbl`. Use the **application-only GBL** for UART installation;
the raw binary's base is `0x4000`, and it is not a complete-chip image.
Packaging/build commands do not contact or flash a device.

## Host radio tools and pinned NCP image

Host commands:

```sh
cd "$WORKSPACE/ziggurat-compilation"
python3 -m venv tools/network-migration-venv
tools/network-migration-venv/bin/pip install \
  bellows==1.0.2 zigpy==2.2.0 universal-silabs-flasher==1.1.0
export MIGRATION_PYTHON="$WORKSPACE/ziggurat-compilation/tools/network-migration-venv/bin/python"
export NCP_IMAGE="$WORKSPACE/ziggurat-compilation/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/firmware/ncp-uart-hw-7.5.1-115200-hw.gbl"
export RCP_IMAGE="$WORKSPACE/ziggurat-compilation/build/firmware-hdlc/build/debug/ot-rcp-multipan.gbl"
cd "$WORKSPACE/microhad-zigbee-native"
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
