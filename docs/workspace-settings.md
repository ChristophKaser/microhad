# Development-host settings

Create one private settings file at the repository root:

```sh
cp workspace.local.env.example workspace.local.env
chmod 600 workspace.local.env
```

Set `GATEWAY_IP`, `LAN_INTERFACE`, compiler selection/paths, and SSH credentials
there. The file is gitignored. Keep local values and passwords out of the example.
This is trusted **POSIX shell code**, sourced with `.`; normal shell quoting,
comments, expansions and commands work. Extra variable assignments are allowed
and ignored by tools that do not use them. Do not source an untrusted file.

| Setting | Purpose / default |
| --- | --- |
| `GATEWAY_IP` | Gateway IP address or hostname; empty until configured |
| `LAN_INTERFACE` | Host LAN/backbone interface used by OTBR diagnostics; empty until configured |
| `TOOLCHAIN` | `rsdk` (default) or experimental `crosstool-ng` |
| `LINK_MODE` | `static` (default) or experimental `dynamic` with RSDK; see [toolchain guide](../config/TOOLCHAINS.md) |
| `RSDK_ROOT` | RSDK installation; default from `config/toolchains.mk` |
| `CROSSTOOL_NG_ROOT` | Candidate compiler installation; default `tools/lexra-proof` |
| `SSH_PASSWORD_FILE` | Existing password file; empty by default |
| `SSH_PASSWORD` | Inline password alternative; empty by default |
| `SSH_USER` | SSH login, default `root` |
| `SSH_PORT` | SSH port, default `22` |
| `SSH_CONNECT_TIMEOUT` | SSH connection timeout in seconds, default `8` |

Use either `SSH_PASSWORD_FILE` or `SSH_PASSWORD`, leaving the other empty. Prefer
referencing an existing password file. Quote inline passwords containing shell
characters, for example `SSH_PASSWORD='a password with $ characters'`. Inline
passwords are written to a private host temporary file for each SSH invocation;
the wrapper removes that file on completion, failure or catchable termination.
Passwords are not passed in command arguments or emitted by the settings bridge.

Relative compiler and configured password-file paths start at the workspace
root, even when invoking a tool from another directory. Explicit command-line
path arguments retain the tool's usual current-directory interpretation.

Explicit tool arguments and Make command-line assignments override environment
variables; environment variables override this file; portable defaults apply
last. Empty environment variables also override the file. Set `WORKSPACE_CONFIG`
to use an alternate file (relative to the workspace root, or absolute). A missing
default file is fine; an explicitly selected missing file is an error.

For example:

```sh
# Uses the selected compiler and credentials from workspace.local.env.
make -C microhad mips

# Explicitly override the compiler for one build.
make -C microhad TOOLCHAIN=crosstool-ng mips

# Only when you intend deployment: normal staging/activation safeguards apply.
./deploy.sh --binary-only --target /tuya/native
```

Shell build helpers, Python host tools, MicroHAD Make, and the Vite development
proxy share these settings. Vite reads `GATEWAY_IP` only when serving the dev
proxy; production bundles do not include the private settings. Neither SDK
installation is needed for ordinary host builds.

Device-only commands use the configured gateway automatically. Tools with both
host and device modes still require an explicit `--host` or `--device` flag;
passing that flag without a value selects `GATEWAY_IP`. Omitting it continues to
select the host fixture. Flash/restore/exclusive-radio confirmations are still
required. Configuration never authorizes deployment or device access by itself.

For interactive shell commands or Docker, load the same settings explicitly:

```sh
# Run from the workspace root.
. ./scripts/workspace-env.sh
workspace_load "$PWD"
printf 'Gateway: %s; LAN: %s\n' "$GATEWAY_IP" "$LAN_INTERFACE"
```

The loader exports non-secret recognized settings. Docker does not inherit host
variables automatically: pass `-e "GATEWAY=$GATEWAY_IP"` and
`-e "BACKBONE_INTERFACE=$LAN_INTERFACE"` as shown in the
[OTBR guide](../scripts/matter-diagnostics/README.md). Never pass the private env
file to Docker with `--env-file`, or copy it into a build context/release archive.

This file configures the development host. It does not replace the gateway's
runtime configuration or change its remote syslog settings.
