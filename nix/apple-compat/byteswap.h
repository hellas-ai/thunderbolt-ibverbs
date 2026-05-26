#ifndef PERFTEST_APPLE_COMPAT_BYTESWAP_H
#define PERFTEST_APPLE_COMPAT_BYTESWAP_H

#include <libkern/OSByteOrder.h>

#include "endian.h"

#define bswap_16(x) OSSwapInt16(x)
#define bswap_32(x) OSSwapInt32(x)
#define bswap_64(x) OSSwapInt64(x)

#endif
