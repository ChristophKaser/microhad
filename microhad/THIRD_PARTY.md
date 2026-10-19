# MicroHAD third-party source

The repository-wide [inventory and attribution requirements](../THIRD_PARTY.md)
cover all components, dependency licenses and release gates. This page retains
MicroHAD-specific build information. Mbed TLS and TF-PSA-Crypto are used under
their Apache-2.0 option; this does not relicense their nested third-party code.

`dependencies/mbedtls` is a Git submodule of
https://github.com/Mbed-TLS/mbedtls.git pinned to `mbedtls-4.2.0`
(commit `ece41aa84d`). Its TF-PSA-Crypto dependency is pinned by upstream
and supplies the cryptographic implementation. Consult each dependency's
LICENSE files for its Apache-2.0 / GPL-2.0-or-later licensing options.

From the repository root, initialize this dependency and its nested submodules:

```sh
git submodule update --init --recursive microhad/dependencies/mbedtls
```

Builds do not fetch dependencies. MicroHAD's TLS and crypto profiles live in
`config/mbedtls_user_config.h` and `config/psa_crypto_config.h`; the upstream
checkout remains unmodified. TLS 1.2, P-256/P-384, RSA signing, AES-GCM,
ChaCha20-Poly1305 and the existing LAN-device authentication hashes are enabled.
PSA keys are volatile; no persistent PSA key store is enabled. PSA initialization
is shared through pthread_once, with upstream pthread synchronization enabled
for concurrent TLS and notification operations. Randomness uses the upstream
OS entropy implementation, with `/dev/urandom` selected for kernels without
`getrandom()`. This preserves the previous TLS entropy source and avoids
blocking startup on the gateway's legacy kernel. Existing launcher seed
restoration remains unchanged.

The project builds static libraries from source for both host tests and the
selected gateway toolchain: RSDK by default, or experimental crosstool-NG via
`TOOLCHAIN=crosstool-ng`. See [toolchain selection](../config/TOOLCHAINS.md).
TLS v4 build outputs use separate `mbedtls-v4` directories to avoid stale v2
CMake caches. Generated files stay in ignored build directories; no host-built
cryptographic object is shipped or linked into the MIPS binary.

The dependency rules use GNU Make 4.3 or newer grouped targets so parallel builds
configure each TLS build directory only once. Dependency HEAD/profile changes
invalidate the archives and consumers; an unchanged build reuses them.

For independent signature and Web Push wire-format checks, install the host-only
Python `cryptography` package and run `make -C microhad crypto-interop-test`.
This additionally checks invalid inputs and concurrent PSA initialization.
