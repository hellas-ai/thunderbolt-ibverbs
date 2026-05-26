#ifndef PERFTEST_APPLE_COMPAT_INFINIBAND_UMAD_H
#define PERFTEST_APPLE_COMPAT_INFINIBAND_UMAD_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static inline int umad_init(void)
{
	return -1;
}

static inline int umad_open_port(char *ca_name, int portnum)
{
	(void)ca_name;
	(void)portnum;
	return -1;
}

static inline int umad_register(int portid, int mgmt_class, int mgmt_version,
				uint32_t rmpp_version, long method_mask)
{
	(void)portid;
	(void)mgmt_class;
	(void)mgmt_version;
	(void)rmpp_version;
	(void)method_mask;
	return -1;
}

static inline void *umad_alloc(int num, size_t size)
{
	return calloc((size_t)num, size);
}

static inline int umad_size(void)
{
	return 0;
}

static inline void *umad_get_mad(void *umad)
{
	return umad;
}

static inline void umad_set_pkey(void *umad, int pkey_index)
{
	(void)umad;
	(void)pkey_index;
}

static inline int umad_set_addr(void *umad, int lid, int qpn, int sl,
				uint32_t qkey)
{
	(void)umad;
	(void)lid;
	(void)qpn;
	(void)sl;
	(void)qkey;
	return -1;
}

static inline int umad_send(int portid, int agentid, void *umad, int length,
			    int timeout_ms, int retries)
{
	(void)portid;
	(void)agentid;
	(void)umad;
	(void)length;
	(void)timeout_ms;
	(void)retries;
	return -1;
}

static inline int umad_recv(int portid, void *umad, int *length,
			    int timeout_ms)
{
	(void)portid;
	(void)umad;
	(void)length;
	(void)timeout_ms;
	return -1;
}

static inline void umad_free(void *umad)
{
	free(umad);
}

static inline int umad_unregister(int portid, int agentid)
{
	(void)portid;
	(void)agentid;
	return 0;
}

static inline int umad_close_port(int portid)
{
	(void)portid;
	return 0;
}

#endif
