# Ziggurat Matter Service

## Host commissioning fixture

Build the synthetic CASE commissioning probe with:

```sh
bash scripts/build-rs-matter-transport.sh host commissioning
```

This writes `build/rs-matter/target/release/ot-matter-commissioning-probe`,
which the native integration fixture executes. A plain Cargo build without the
repository's `CARGO_TARGET_DIR` writes under `rs-matter-probe/target/` and leaves
the native fixture using its previous binary. The host build uses a synthetic
RCP/accessory and does not deploy or access the Tuya gateway.

The production MIPS service uses the separate command
`bash scripts/build-rs-matter-transport.sh mips service`. That cross-build is a
preparation step only; deploy only through the native repository's documented
deployment flow after its flash preflight and required backup checks.
