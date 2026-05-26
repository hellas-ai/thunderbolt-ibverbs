#ifndef PERFTEST_APPLE_COMPAT_ENDIAN_H
#define PERFTEST_APPLE_COMPAT_ENDIAN_H

#include <libkern/OSByteOrder.h>
#include <machine/endian.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER __DARWIN_BYTE_ORDER
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __DARWIN_LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __DARWIN_BIG_ENDIAN
#endif

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)

#ifndef s6_addr32
#define s6_addr32 __u6_addr.__u6_addr32
#endif

#endif
