#ifndef PERFTEST_APPLE_COMPAT_MALLOC_H
#define PERFTEST_APPLE_COMPAT_MALLOC_H

#include <alloca.h>
#include <stdlib.h>
#include <string.h>

#ifndef SHM_HUGETLB
#define SHM_HUGETLB 0
#endif
#ifndef _SC_LEVEL1_DCACHE_LINESIZE
#define _SC_LEVEL1_DCACHE_LINESIZE (-1)
#endif

static inline void *memalign(size_t alignment, size_t size)
{
	void *ptr = NULL;

	if (posix_memalign(&ptr, alignment, size) != 0)
		return NULL;

	return ptr;
}

#ifndef strdupa
#define strdupa(s) strcpy((char *)alloca(strlen(s) + 1), (s))
#endif

#endif
