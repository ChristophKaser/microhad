#ifndef MICROHAD_MATTER_CBOR_PLATFORM_H
#define MICROHAD_MATTER_CBOR_PLATFORM_H

// GCC 4.4 has bswap builtins but lacks the newer endian macros TinyCBOR uses.
// Supply them from libc so undefined macros cannot select the little-endian path.
#include <endian.h>
#ifndef __BYTE_ORDER__
#define __BYTE_ORDER__ __BYTE_ORDER
#endif
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ __LITTLE_ENDIAN
#endif
#ifndef __ORDER_BIG_ENDIAN__
#define __ORDER_BIG_ENDIAN__ __BIG_ENDIAN
#endif

#endif
