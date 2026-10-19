# Install a binary release

The Linux-host installer uploads applications over SSH. No compiler, Rust, Node,
source checkout or MIPS toolchain is needed on the installation computer. These
static crosstool-NG builds are experimental on the Silvercrest SGWZ 1 A1 / HG06339
(RTL8196E, EFR32MG1B232, factory Linux); they are not universal MIPS packages.

The same bundle supports two modes:

| Installer mode | Applications | Radio prerequisite |
| --- | --- | --- |
| `microhad-only` | MicroHAD EZSP daemon and Web UI | Compatible stock EZSP NCP firmware; no radio replacement by this installer |
| `paired` (default) | MicroHAD, Ziggurat multipan and Matter daemon | Separately installed compatible two-instance HDLC RCP firmware and prepared Zigbee state |

Radio firmware and Android APKs are **not** included. Switching between modes
requires the documented radio/network migration procedure. The installer refuses
backend changes during updates; it does not flash, reset or form a replacement
radio network.

## Before installing

For a first installation, complete the root-access, SSH host-key verification,
backups, idle-launcher and radio preparation steps in the repository's
`docs/installation.md`. Keep UART recovery access. For an existing Zigbee network,
follow `docs/ziggurat-deployment-plan.md` before a handover. Binary packages replace
application compilation, not those prerequisites. The gateway must already have
`/tuya/tuya_start.sh`, `/dev/ttyS1`, a working clock setup and root SSH access.

Use a Linux computer with Python 3.11+, OpenSSH client, `sshpass`, `curl`, binutils
(`readelf`, `strings`), `tar`, `xz`, and standard checksum utilities. For example,
on Debian/Ubuntu:

```sh
sudo apt install python3 openssh-client sshpass curl binutils tar xz-utils
```

Store the gateway password in a private file outside the extracted package,
with mode `0600`. Verify the gateway SSH host key through a trusted channel and
add it to your known-hosts file; the installer deliberately requires an existing
verified host key. Use `--ssh-port 2333` only if that is your verified device port.

## Download and verify

Download `microhad-VERSION-lexra-static.tar.xz`, `release-manifest.json` and
`SHA256SUMS` from the same release. The matching source archive is available
alongside them. Verify each downloaded file against its checksum (if both
archives are downloaded, `sha256sum -c SHA256SUMS` verifies the whole set):

```sh
sha256sum microhad-VERSION-lexra-static.tar.xz
# Compare with its entry in SHA256SUMS from the trusted release.
tar -xJf microhad-VERSION-lexra-static.tar.xz
cd microhad-VERSION-lexra-static
./install.sh check --mode microhad-only --host 192.168.1.50 \
  --password-file /private/gateway-password
```

Checksums detect corruption; obtain the manifest/checksums through the trusted
release channel. The installer also checks every packaged file and ELF before
making changes. `check` only performs local checks and read-only remote preflight.
The installer can be invoked from any working directory.

## First installation

Copy the example matching your backend to a private location and edit it. Keep
`runtime_dir` at `/tuya/native`; configure ports, gateway name and any private TLS
or notification inputs separately. Do not include `ziggurat_socket` for EZSP.

For MicroHAD with compatible stock radio firmware:

```sh
cp config.microhad-only.example.json /private/microhad-config.json
# Edit /private/microhad-config.json, then:
./install.sh install --mode microhad-only --host 192.168.1.50 \
  --password-file /private/gateway-password --config /private/microhad-config.json
```

For a prepared Ziggurat/Matter gateway:

```sh
cp config.paired.example.json /private/microhad-config.json
# Edit the configuration; obtain prepared state from the migration/installation guide.
./install.sh install --mode paired --host 192.168.1.50 \
  --password-file /private/gateway-password --config /private/microhad-config.json \
  --ziggurat-state /private/prepared-native-ziggurat-state.json
```

Initial configuration/state are accepted only when absent. The temporary
maintenance helper checks exclusive UART ownership before initial upload. The
installer refuses an existing configuration rather than overwriting it.

## Update an installed gateway

Use the same mode as the existing installation:

```sh
./install.sh update --mode microhad-only --host 192.168.1.50 \
  --password-file /private/gateway-password
# Or use --mode paired for Ziggurat/Matter.
```

Updates retain configuration, Zigbee/Thread state, Matter identity, TLS material
and existing backups. They stop the recognized services, transfer verified
artifacts, update the launcher and start the stack. A 512 KiB flash reserve is
retained; the package is not unpacked onto the gateway.

Rollback copies of application files are opt-in:

```sh
./install.sh update --mode paired --host 192.168.1.50 \
  --password-file /private/gateway-password --rollback-dir /private/empty-rollback-dir
```

The rollback directory must be empty and private. Identity/network state is never
included in these application backups. There is no automatic rollback. If a
transfer/startup fails, inspect service state using SSH/UART and restore the
saved application files according to `docs/recovery-tooling.md`. Preserve all
network/identity files. A partially completed first installation may already
have configuration/state present; inspect it instead of deleting those files
or forcing another first installation. A running deleted executable or
unrecognized supervisor must be resolved before retrying.

After success open `http://GATEWAY/`, check device readings and control, and use
“Open-source licenses” for notices. Set up Thread/Matter through the dashboard
only in paired mode. No standalone license files are installed on the gateway.

## Build a release or rebuild its source package

From a committed repository, on Linux x86_64:

```sh
python3 scripts/build-release.py --version VERSION --jobs 4
```

The build uses crosstool-NG and static linking regardless of private workspace
settings. It builds both MicroHAD modes, Ziggurat multipan, the Matter service,
helpers and Web UI. Output appears under `build/releases/VERSION/`; unfinished
workspaces retain the `.incomplete` suffix. Existing output is never overwritten.
The builder does not install, deploy, publish, or access a gateway.

The matching source archive includes pinned dependencies, patched source trees,
Cargo/npm sources, Rust standard-library sources, compiler/libc source archives,
configurations, licenses and a checksummed source manifest. It excludes radio
firmware, proprietary SDK bundles, Git history, local settings and device data.
The original application sources remain MIT; upstream libraries retain their
licenses, including LGPL 2.1 for uClibc-ng and GCC runtime exceptions.

Install the Linux host prerequisites described in `config/TOOLCHAINS.md`
(GCC/G++, Make, CMake, Ninja, autoconf/automake, flex, bison, gperf, makeinfo,
rsync, patch, Python 3.11+, Node/npm, LLVM 18 tools and development libraries).
Install the **exact** Rust nightly from `config/toolchains.mk`, with `rust-src`,
before disconnecting networking. The compiler version is checked against the
release manifest. The source archive supplies the standard-library source used
by the rebuild; a separately installed compiler supplies the compiler executable.
No preinstalled MIPS compiler is required; the bundled compiler/libc sources are
built locally (allow at least 12 GiB free space).

```sh
tar -xJf microhad-VERSION-source.tar.xz
cd microhad-VERSION-source
./rebuild.sh --cargo-home "$HOME/.cargo" --rustup-home "$HOME/.rustup" \
  --output /private/rebuilt-VERSION --jobs 4
```

Rebuilding is offline and validates source hashes before compiling. To exercise
your LGPL right to modify uClibc-ng, edit the bundled library source archive or
configuration, update the corresponding source checksum and source-manifest
entry, and rebuild the toolchain/applications. Reapply any required Lexra patches.
This deliberately creates a modified build, not an unchanged official release.
Keep original library copyright/license notices and identify your modifications.
The build does not impose byte-for-byte equality with published executable files.
