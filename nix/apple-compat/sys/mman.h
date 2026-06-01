#ifndef PERFTEST_APPLE_COMPAT_SYS_MMAN_H
#define PERFTEST_APPLE_COMPAT_SYS_MMAN_H

#include_next <sys/mman.h>

#ifdef __APPLE__
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#endif
