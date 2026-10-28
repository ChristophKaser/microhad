# Embedded home automation

This repository contains the complete gateway software stack. `microhad/` is
the native C++03 daemon and Web UI; `ziggurat/`, `micro-thread-matter-daemon/` and
`firmware/` own their respective source, scripts, patches and dependencies.
Gecko SDK belongs to firmware under `firmware/dependencies/`; Thread/Matter
owns its upstream OpenThread checkout. Toolchains remain at the root.
See [microhad/AGENTS.md](microhad/AGENTS.md) for native daemon, deployment, and
device-state requirements. Its C++ and Web UI verification rules apply to all
changes in that component.

## Private development-host settings

Use the gitignored root `workspace.local.env`, with
`workspace.local.env.example` as the tracked template. It supplies gateway IP,
LAN interface, compiler mode/paths and SSH credentials to host tools. See
[shared host settings](docs/workspace-settings.md). Existing environment and
explicit tool/Make arguments override it; extra keys are allowed. It is trusted
POSIX shell code, sourced by `scripts/workspace-env.sh`, not a device config.
Never commit it or print credentials. Prefer `SSH_PASSWORD_FILE` by pathname.

Host tools load it automatically. Before raw host shell commands such as curl
or Docker, source the loader and call `workspace_load "$PWD"` from the root.
Do not infer device-access authorization from configured defaults. Optional
device tests still require explicit `--host` or `--device`.

## Shared toolchains and builds

Compiler defaults are maintained once in `config/toolchains.mk`. Shell build
helpers source `scripts/toolchains.sh`; Python checks use
`scripts/toolchain_config.py`; MicroHAD's Makefile includes the same settings.
`RSDK_ROOT` can be set in `workspace.local.env`, the environment or on a Make command line to select
another compatible toolchain. `TOOLCHAIN=crosstool-ng` selects the source-built
Lexra/uClibc-ng compiler; `CROSSTOOL_NG_ROOT` overrides its location.
See [toolchain selection](config/TOOLCHAINS.md) for build commands, isolated outputs
and compatibility limits. RSDK remains the default.

Initialize the pinned shared C dependencies before building either gateway service:

```sh
git submodule update --init third-party/nanopb third-party/acetimec/upstream
```

AceTime's offsets-only mailbox is applied in build output; keep both dependency
checkouts unmodified. Generated timezone subsets stay in `third-party/acetimec/`.

From the repository root, build MicroHAD with:

```sh
make -C microhad host test
make -C microhad mips
```

Build the synthetic CASE probe with
`bash micro-thread-matter-daemon/scripts/build.sh host commissioning`. This writes
`build/micro-thread-matter-daemon/target/release/ot-matter-commissioning-probe`; do not run a
plain Cargo build because it would leave the integration fixture using an old
binary. The fixture uses synthetic radio/accessory traffic and does not access
the gateway.

Build the target Ziggurat and Matter service with:

```sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash micro-thread-matter-daemon/scripts/build.sh mips service
```

For MicroHAD updates, use `./deploy.sh` after building the required
artifacts. Ordinary deployment stages files; launcher activation and service
restart are separate operations. Matter updates use the documented maintenance deployment flow in
[microhad/AGENTS.md](microhad/AGENTS.md): `--services matter` uploads only a
changed Matter service; complete releases use `--maintenance-deploy`. Rollback
artifact generation is disabled unless `--rollback-dir DIR` is supplied.
Never deploy to a device unless explicitly requested.

## Repository and device state

Generated artifacts under `build/`, Rust targets, tools, logs, target
specifications, and worktrees are ignored. `garage-app/` is also
intentionally ignored; it is a separate Android project with local credentials
and its own Git metadata. Do not stage or commit its contents. Never commit
device configuration, credentials, production TLS keys, Matter identity,
persistent network state, or Zigbee keys.

Keep transient gateway output under `/tmp`, not `/tuya` flash. Preserve gateway
configuration, Matter identity, Zigbee network state, and existing backups.
Deployment and activation must follow the safeguards documented in
`microhad/AGENTS.md`; do not run deployment scripts against a device without an
explicit request.
