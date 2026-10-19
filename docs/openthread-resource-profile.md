# Measure OpenThread resources

Use the synthetic CLI probe to characterize a particular build on factory MIPS
Linux. It measures internal UDP and simulated RCP calls; it does not measure
radio throughput, accessory behavior or the full Matter service.

## Run a profile

Build the CLI and relay using [OpenThread build commands](openthread-feasibility.md).
Configure the gateway and SSH credentials in the private
[workspace env file](workspace-settings.md). For an explicitly requested device
test, run from the repository root (`--host` selects the configured gateway):

```sh
python3 micro-thread-matter-daemon/scripts/test-openthread-device.py \
  --host --profile
```

The default profile uses 20 seconds disabled idle, 60 seconds load and 20 seconds
enabled idle. `--profile-load-seconds` accepts 5–120. The
`--constrained-memory-profile` option requires both `--host` and `--profile`.

The runner checks production online/formed status, stages distinct probe/relay
artifacts, uses private temporary settings and a synthetic socket, and checks
cleanup. It does not access `/dev/ttyS1`. Standard preflight requires 3072 KiB
free memory; constrained preflight requires 2560 KiB. RSS guards are 2048/1024 KiB
and runtime free-memory guards are 1024/1536 KiB respectively. Consult the runner
when changing those limits rather than carrying forward an old measurement.

## Interpret results

Review sampled CPU, RSS, minimum free memory, latency, cycle count, production
status before/after and cleanup results. One-second sampling can miss brief
peaks. Synthetic cycle latency includes SSH/relay delivery and is not RF latency.
A run passing just above its memory threshold establishes little spare capacity.

For a release, measure the complete native/Ziggurat/Matter workload under
sustained accessory reports and control traffic, including restart/recovery.
CLI figures cannot substitute for that check. Record exact commits, hashes,
workload and limits with every result; measure the selected release rather than
using an older build's figures.
