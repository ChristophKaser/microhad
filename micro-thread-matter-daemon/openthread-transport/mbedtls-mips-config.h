// Keep the pinned OpenThread configuration, using portable crypto on Lexra.
#ifndef GATEWAY_MBEDTLS_MIPS_CONFIG_H
#define GATEWAY_MBEDTLS_MIPS_CONFIG_H

#include "openthread-mbedtls-config.h"

// With this MIPS-I/Lexra toolchain, Mbed TLS's assembly arithmetic rejects a
// valid P-256 public key (ECP_INVALID_KEY). The same signed SRP update verifies
// on this CPU with portable arithmetic. Apply consistently to every crypto TU.
#undef MBEDTLS_HAVE_ASM

#endif
