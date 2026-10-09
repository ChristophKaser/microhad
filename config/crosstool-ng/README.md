# Toolchain configuration provenance

The Lexra architecture and GCC/binutils selection are adapted from
[jnilo1/rtl8196e-gateway](https://github.com/jnilo1/rtl8196e-gateway), specifically
its [crosstool-NG configuration](https://github.com/jnilo1/rtl8196e-gateway/blob/30eb5e5b9ee334d2c6d76812acd4a6cdf2923423/1-Build-Environment/10-lexra-toolchain/crosstool-ng.config)
at commit `30eb5e5b9ee334d2c6d76812acd4a6cdf2923423`.
Credit goes to that project's maintainers and contributors for the upstream
Lexra toolchain configuration and patch set.

The builder also uses that revision's [GCC Lexra patch](https://github.com/jnilo1/rtl8196e-gateway/blob/30eb5e5b9ee334d2c6d76812acd4a6cdf2923423/1-Build-Environment/10-lexra-toolchain/patches/gcc/15.2.0/970-Lexra.patch)
and [binutils Lexra patch](https://github.com/jnilo1/rtl8196e-gateway/blob/30eb5e5b9ee334d2c6d76812acd4a6cdf2923423/1-Build-Environment/10-lexra-toolchain/patches/binutils/2.45.1/600-Lexra.patch),
read from the pinned checkout under `firmware/dependencies/rtl8196e-gateway`.
It records their hashes and the checkout revision in the generated source manifest.

This repository adapts the upstream musl profile to the gateway's stock Linux
3.10.90 kernel using uClibc-ng 1.0.54 with LinuxThreads. The `uclibc.config`
profile is maintained here: native TLS, time64 and FENV are disabled, and legacy
APIs and netlink interface enumeration are enabled for gateway applications.
Build/install paths and the bootstrap process are also local adaptations.
The builder uses the Lexra patches from the upstream set; its Alpine-specific
GCC patches are not selected by this profile.

See [gateway toolchains](../TOOLCHAINS.md) for build commands and validation limits.
