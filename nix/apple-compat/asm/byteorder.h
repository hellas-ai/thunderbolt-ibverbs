#ifndef PERFTEST_APPLE_COMPAT_ASM_BYTEORDER_H
#define PERFTEST_APPLE_COMPAT_ASM_BYTEORDER_H

#include <stdint.h>

#include "endian.h"

typedef uint8_t __u8;
typedef uint16_t __be16;

#if BYTE_ORDER == LITTLE_ENDIAN
#define __LITTLE_ENDIAN_BITFIELD
#elif BYTE_ORDER == BIG_ENDIAN
#define __BIG_ENDIAN_BITFIELD
#else
#error "Must set BYTE_ORDER"
#endif

#endif
