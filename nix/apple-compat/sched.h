#ifndef PERFTEST_APPLE_COMPAT_SCHED_H
#define PERFTEST_APPLE_COMPAT_SCHED_H

#include_next <sched.h>

#ifdef __APPLE__
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#endif

typedef struct {
	unsigned long bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

static inline void CPU_ZERO(cpu_set_t *set)
{
	memset(set, 0, sizeof(*set));
}

static inline void CPU_SET(int cpu, cpu_set_t *set)
{
	if (cpu >= 0 && cpu < CPU_SETSIZE)
		set->bits[cpu / (8 * sizeof(unsigned long))] |=
			1UL << (cpu % (8 * sizeof(unsigned long)));
}

static inline int CPU_COUNT(const cpu_set_t *set)
{
	int count = 0;

	for (size_t i = 0; i < sizeof(set->bits) / sizeof(set->bits[0]); i++)
		count += __builtin_popcountl(set->bits[i]);

	return count;
}

static inline int sched_setaffinity(pid_t pid, size_t cpusetsize,
				    const cpu_set_t *mask)
{
	(void)pid;
	(void)cpusetsize;
	(void)mask;
	errno = ENOTSUP;
	return -1;
}

static inline int sched_getaffinity(pid_t pid, size_t cpusetsize,
				    cpu_set_t *mask)
{
	(void)pid;
	(void)cpusetsize;
	CPU_ZERO(mask);
	errno = ENOTSUP;
	return -1;
}

static inline int sched_getcpu(void)
{
	return -1;
}
#endif

#endif
