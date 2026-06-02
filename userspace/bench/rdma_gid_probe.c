// SPDX-License-Identifier: MIT
/*
 * Probe whether common LLM runtimes can match an RDMA GID automatically.
 *
 * llama.cpp's RPC RDMA transport builds a RoCE-shaped target GID from the
 * connected TCP socket's local address, scans libibverbs devices on port 1,
 * and picks the first device with a matching RoCEv2 GID, falling back to
 * RoCEv1. This tool mirrors that selection rule so the Thunderbolt RDMA
 * netdev/GID/routing setup can be tested directly.
 *
 * NCCL/RCCL, used by vLLM tensor/pipeline parallelism, has a different rule:
 * when NCCL_IB_GID_INDEX is left unset it dynamically picks a non-link-local
 * GID matching the configured address family, RoCE version, and optional CIDR
 * range. This tool can mirror that rule too.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

enum probe_mode {
	MODE_LLAMA,
	MODE_NCCL,
};

struct options {
	enum probe_mode mode;
	const char *local_ip;
	const char *connect_host;
	const char *connect_port;
	const char *dev_filter;
	uint32_t ib_port;
	int explicit_gid;
	int connect_timeout_sec;
	int nccl_addr_family;
	int nccl_roce_version;
	bool nccl_has_addr_range;
	union {
		struct in_addr v4;
		struct in6_addr v6;
	} nccl_addr_range;
	int nccl_addr_range_bits;
	bool fail_on_ambiguous;
};

struct candidate {
	bool found;
	char dev_name[128];
	int gid_index;
	uint32_t gid_type;
	int score;
	int dev_order;
};

static void usage(FILE *out, const char *argv0)
{
	fprintf(out,
		"usage: %s [--mode llama|nccl] [--local-ip ADDRESS | --connect HOST PORT] [options]\n"
		"\n"
		"Options:\n"
		"  --dev NAME             restrict to one RDMA device\n"
		"  --port N               verbs port number (default: 1)\n"
		"  --gid N                inspect explicit GID index like GGML_RDMA_GID\n"
		"  --connect-timeout SEC  TCP connect timeout (default: 5)\n"
		"  --nccl-addr-family F   NCCL dynamic GID family: AF_INET or AF_INET6\n"
		"  --nccl-addr-range CIDR NCCL dynamic GID CIDR filter\n"
		"  --nccl-roce-version N  NCCL dynamic RoCE version (default: 2)\n"
		"  --fail-on-ambiguous    fail if more than one device matches\n",
		argv0);
}

static int parse_u32(const char *text, uint32_t *out)
{
	char *end = NULL;
	unsigned long val;

	errno = 0;
	val = strtoul(text, &end, 0);
	if (errno || !end || *end || val > UINT32_MAX)
		return -1;

	*out = (uint32_t)val;
	return 0;
}

static int parse_family(const char *text, int *family)
{
	if (!strcmp(text, "AF_INET") || !strcmp(text, "inet") ||
	    !strcmp(text, "ipv4") || !strcmp(text, "4")) {
		*family = AF_INET;
		return 0;
	}
	if (!strcmp(text, "AF_INET6") || !strcmp(text, "inet6") ||
	    !strcmp(text, "ipv6") || !strcmp(text, "6")) {
		*family = AF_INET6;
		return 0;
	}
	return -1;
}

static int parse_cidr(const char *text, struct options *opts)
{
	char buf[128];
	char *slash;
	uint32_t bits;

	if (strlen(text) >= sizeof(buf))
		return -1;
	snprintf(buf, sizeof(buf), "%s", text);

	slash = strchr(buf, '/');
	if (!slash)
		return -1;
	*slash++ = '\0';

	if (parse_u32(slash, &bits))
		return -1;

	if (opts->nccl_addr_family == AF_INET) {
		if (bits > 32)
			return -1;
		if (inet_pton(AF_INET, buf, &opts->nccl_addr_range.v4) != 1)
			return -1;
	} else if (opts->nccl_addr_family == AF_INET6) {
		if (bits > 128)
			return -1;
		if (inet_pton(AF_INET6, buf, &opts->nccl_addr_range.v6) != 1)
			return -1;
	} else {
		return -1;
	}

	opts->nccl_has_addr_range = true;
	opts->nccl_addr_range_bits = (int)bits;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *opts)
{
	opts->mode = MODE_LLAMA;
	opts->ib_port = 1;
	opts->explicit_gid = -1;
	opts->connect_timeout_sec = 5;
	opts->nccl_addr_family = AF_INET;
	opts->nccl_roce_version = 2;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			const char *mode = argv[++i];

			if (!strcmp(mode, "llama"))
				opts->mode = MODE_LLAMA;
			else if (!strcmp(mode, "nccl") || !strcmp(mode, "rccl"))
				opts->mode = MODE_NCCL;
			else
				return -1;
		} else if (!strcmp(argv[i], "--local-ip") && i + 1 < argc) {
			opts->local_ip = argv[++i];
		} else if (!strcmp(argv[i], "--connect") && i + 2 < argc) {
			opts->connect_host = argv[++i];
			opts->connect_port = argv[++i];
		} else if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			opts->dev_filter = argv[++i];
		} else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			if (parse_u32(argv[++i], &opts->ib_port))
				return -1;
		} else if (!strcmp(argv[i], "--gid") && i + 1 < argc) {
			uint32_t gid;

			if (parse_u32(argv[++i], &gid) || gid > INT32_MAX)
				return -1;
			opts->explicit_gid = (int)gid;
		} else if (!strcmp(argv[i], "--connect-timeout") &&
			   i + 1 < argc) {
			uint32_t timeout;

			if (parse_u32(argv[++i], &timeout) || timeout > INT32_MAX)
				return -1;
			opts->connect_timeout_sec = (int)timeout;
		} else if (!strcmp(argv[i], "--nccl-addr-family") &&
			   i + 1 < argc) {
			if (parse_family(argv[++i], &opts->nccl_addr_family))
				return -1;
		} else if (!strcmp(argv[i], "--nccl-addr-range") &&
			   i + 1 < argc) {
			if (parse_cidr(argv[++i], opts))
				return -1;
		} else if (!strcmp(argv[i], "--nccl-roce-version") &&
			   i + 1 < argc) {
			uint32_t version;

			if (parse_u32(argv[++i], &version) ||
			    version < 1 || version > 2)
				return -1;
			opts->nccl_roce_version = (int)version;
		} else if (!strcmp(argv[i], "--fail-on-ambiguous")) {
			opts->fail_on_ambiguous = true;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(stdout, argv[0]);
			exit(0);
		} else {
			return -1;
		}
	}

	if (opts->mode == MODE_LLAMA &&
	    (opts->local_ip != NULL) == (opts->connect_host != NULL))
		return -1;
	if (opts->mode == MODE_NCCL && opts->local_ip)
		return -1;

	return 0;
}

static void print_gid_raw(const union ibv_gid *gid)
{
	for (int i = 0; i < 16; i += 2)
		printf("%02x%02x%s", gid->raw[i], gid->raw[i + 1],
		       i == 14 ? "" : ":");
}

static void print_gid_addr(const union ibv_gid *gid)
{
	char buf[INET6_ADDRSTRLEN];
	bool v4_mapped = true;

	for (int i = 0; i < 10; i++) {
		if (gid->raw[i] != 0) {
			v4_mapped = false;
			break;
		}
	}
	v4_mapped = v4_mapped && gid->raw[10] == 0xff && gid->raw[11] == 0xff;

	if (v4_mapped && inet_ntop(AF_INET, &gid->raw[12], buf, sizeof(buf))) {
		printf("::ffff:%s", buf);
		return;
	}
	if (inet_ntop(AF_INET6, gid->raw, buf, sizeof(buf))) {
		printf("%s", buf);
		return;
	}
	print_gid_raw(gid);
}

static const char *gid_type_name(uint32_t gid_type)
{
	switch (gid_type) {
	case IBV_GID_TYPE_IB:
		return "ib";
	case IBV_GID_TYPE_ROCE_V1:
		return "roce-v1";
	case IBV_GID_TYPE_ROCE_V2:
		return "roce-v2";
	default:
		return "unknown";
	}
}

static int gid_type_score(uint32_t gid_type)
{
	switch (gid_type) {
	case IBV_GID_TYPE_ROCE_V2:
		return 2;
	case IBV_GID_TYPE_ROCE_V1:
		return 1;
	default:
		return 0;
	}
}

static int gid_roce_version(uint32_t gid_type)
{
	switch (gid_type) {
	case IBV_GID_TYPE_ROCE_V1:
		return 1;
	case IBV_GID_TYPE_ROCE_V2:
		return 2;
	default:
		return 0;
	}
}

static int gid_addr_family(const union ibv_gid *gid)
{
	bool v4_mapped = true;

	for (int i = 0; i < 10; i++) {
		if (gid->raw[i] != 0) {
			v4_mapped = false;
			break;
		}
	}
	v4_mapped = v4_mapped && gid->raw[10] == 0xff && gid->raw[11] == 0xff;
	return v4_mapped ? AF_INET : AF_INET6;
}

static bool gid_is_zero(const union ibv_gid *gid)
{
	for (int i = 0; i < 16; i++) {
		if (gid->raw[i])
			return false;
	}
	return true;
}

static bool gid_is_link_local(const union ibv_gid *gid)
{
	return gid->raw[0] == 0xfe && (gid->raw[1] & 0xc0) == 0x80;
}

static bool gid_is_valid_global(const union ibv_gid *gid)
{
	return !gid_is_zero(gid) && !gid_is_link_local(gid);
}

static bool prefix_matches_bytes(const uint8_t *addr, const uint8_t *prefix,
				 int bits, int max_bits)
{
	int full_bytes;
	int rem_bits;

	if (bits < 0 || bits > max_bits)
		return false;

	full_bytes = bits / 8;
	rem_bits = bits % 8;

	if (full_bytes && memcmp(addr, prefix, (size_t)full_bytes) != 0)
		return false;
	if (rem_bits) {
		uint8_t mask = (uint8_t)(0xffu << (8 - rem_bits));

		if ((addr[full_bytes] & mask) != (prefix[full_bytes] & mask))
			return false;
	}
	return true;
}

static bool nccl_gid_matches_range(const struct options *opts,
				   const union ibv_gid *gid)
{
	const uint8_t *addr;
	const uint8_t *prefix;
	int max_bits;

	if (!opts->nccl_has_addr_range)
		return true;

	if (opts->nccl_addr_family == AF_INET) {
		addr = &gid->raw[12];
		prefix = (const uint8_t *)&opts->nccl_addr_range.v4;
		max_bits = 32;
	} else {
		addr = gid->raw;
		prefix = (const uint8_t *)&opts->nccl_addr_range.v6;
		max_bits = 128;
	}

	return prefix_matches_bytes(addr, prefix,
				    opts->nccl_addr_range_bits, max_bits);
}

static int gid_from_sockaddr(const struct sockaddr *addr, union ibv_gid *gid)
{
	memset(gid, 0, sizeof(*gid));

	if (addr->sa_family == AF_INET) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

		gid->raw[10] = 0xff;
		gid->raw[11] = 0xff;
		memcpy(&gid->raw[12], &sin->sin_addr, sizeof(sin->sin_addr));
		return 0;
	}

	if (addr->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;

		memcpy(gid->raw, &sin6->sin6_addr, sizeof(sin6->sin6_addr));
		return 0;
	}

	return -1;
}

static int gid_from_ip(const char *ip, union ibv_gid *gid)
{
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &sin.sin_addr) == 1)
		return gid_from_sockaddr((const struct sockaddr *)&sin, gid);

	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;
	if (inet_pton(AF_INET6, ip, &sin6.sin6_addr) == 1)
		return gid_from_sockaddr((const struct sockaddr *)&sin6, gid);

	return -1;
}

static int connect_with_timeout(int fd, const struct sockaddr *addr,
				socklen_t addrlen, int timeout_sec)
{
	int flags;
	int ret;
	int err = 0;
	socklen_t err_len = sizeof(err);
	fd_set wfds;
	struct timeval timeout;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return connect(fd, addr, addrlen);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	ret = connect(fd, addr, addrlen);
	if (!ret)
		goto restore;
	if (errno != EINPROGRESS)
		return -1;

	FD_ZERO(&wfds);
	FD_SET(fd, &wfds);
	timeout.tv_sec = timeout_sec;
	timeout.tv_usec = 0;

	ret = select(fd + 1, NULL, &wfds, NULL, &timeout);
	if (ret == 0) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (ret < 0)
		return -1;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0)
		return -1;
	if (err) {
		errno = err;
		return -1;
	}

restore:
	return fcntl(fd, F_SETFL, flags);
}

static int connected_socket_gid(const char *host, const char *port,
				int timeout_sec, union ibv_gid *gid)
{
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res = NULL;
	struct addrinfo *ai;
	int fd = -1;
	int gai;
	int ret = -1;

	gai = getaddrinfo(host, port, &hints, &res);
	if (gai) {
		fprintf(stderr, "getaddrinfo(%s,%s): %s\n", host, port,
			gai_strerror(gai));
		return -1;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		struct sockaddr_storage local;
		socklen_t local_len = sizeof(local);

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen,
					 timeout_sec)) {
			close(fd);
			fd = -1;
			continue;
		}
		if (getsockname(fd, (struct sockaddr *)&local, &local_len) == 0)
			ret = gid_from_sockaddr((const struct sockaddr *)&local, gid);
		close(fd);
		break;
	}

	freeaddrinfo(res);
	return ret;
}

static void print_target(const union ibv_gid *target)
{
	printf("target_gid=");
	print_gid_addr(target);
	printf(" raw=");
	print_gid_raw(target);
	printf("\n");
}

static void print_nccl_config(const struct options *opts)
{
	char prefix[INET6_ADDRSTRLEN];

	printf("nccl_dynamic_config family=%s roce_version=%d",
	       opts->nccl_addr_family == AF_INET ? "AF_INET" : "AF_INET6",
	       opts->nccl_roce_version);
	if (opts->nccl_has_addr_range) {
		if (opts->nccl_addr_family == AF_INET)
			inet_ntop(AF_INET, &opts->nccl_addr_range.v4,
				  prefix, sizeof(prefix));
		else
			inet_ntop(AF_INET6, &opts->nccl_addr_range.v6,
				  prefix, sizeof(prefix));
		printf(" addr_range=%s/%d", prefix,
		       opts->nccl_addr_range_bits);
	}
	printf("\n");
}

static void print_match(const char *dev_name, uint32_t ib_port,
			const struct ibv_gid_entry *entry,
			bool explicit_gid, bool selected)
{
	printf("%s dev=%s port=%u gid=%u type=%s ifindex=%u gid_addr=",
	       selected ? "selected" : "match", dev_name, ib_port,
	       entry->gid_index, gid_type_name(entry->gid_type),
	       entry->ndev_ifindex);
	print_gid_addr(&entry->gid);
	printf(" raw=");
	print_gid_raw(&entry->gid);
	if (explicit_gid)
		printf(" explicit");
	printf("\n");
}

static void maybe_set_selected(struct candidate *selected,
			       const struct candidate *cand)
{
	if (!cand->found)
		return;
	if (!selected->found || cand->dev_order < selected->dev_order)
		*selected = *cand;
}

static int scan_devices(const struct options *opts, const union ibv_gid *target)
{
	struct ibv_device **devs;
	struct candidate selected = {0};
	int num_devs = 0;
	int matching_devices = 0;
	int matching_entries = 0;

	devs = ibv_get_device_list(&num_devs);
	if (!devs || num_devs == 0) {
		fprintf(stderr, "no libibverbs devices found\n");
		return 1;
	}

	for (int d = 0; d < num_devs; d++) {
		const char *dev_name = ibv_get_device_name(devs[d]);
		struct ibv_context *ctx;
		struct ibv_port_attr port_attr;
		struct candidate dev_best = {0};
		int dev_entries = 0;

		if (opts->dev_filter && strcmp(opts->dev_filter, dev_name))
			continue;

		ctx = ibv_open_device(devs[d]);
		if (!ctx) {
			fprintf(stderr, "open %s: %s\n", dev_name,
				strerror(errno));
			continue;
		}

		memset(&port_attr, 0, sizeof(port_attr));
		if (ibv_query_port(ctx, opts->ib_port, &port_attr)) {
			fprintf(stderr, "query_port %s/%u: %s\n", dev_name,
				opts->ib_port, strerror(errno));
			ibv_close_device(ctx);
			continue;
		}

		if (opts->explicit_gid >= 0) {
			struct ibv_gid_entry entry;

			memset(&entry, 0, sizeof(entry));
			if (ibv_query_gid_ex(ctx, opts->ib_port,
					     (uint32_t)opts->explicit_gid,
					     &entry, 0) == 0) {
				bool bytes_match =
					memcmp(entry.gid.raw, target->raw,
					       sizeof(target->raw)) == 0;

				print_match(dev_name, opts->ib_port, &entry,
					    true, true);
				if (!bytes_match)
					printf("warning dev=%s gid=%u does not match target_gid\n",
					       dev_name, entry.gid_index);
				dev_best.found = true;
				snprintf(dev_best.dev_name, sizeof(dev_best.dev_name),
					 "%s", dev_name);
				dev_best.gid_index = (int)entry.gid_index;
				dev_best.gid_type = entry.gid_type;
				dev_best.score = gid_type_score(entry.gid_type);
				dev_best.dev_order = d;
				dev_entries = 1;
			}
		} else {
			for (uint32_t i = 0; i < port_attr.gid_tbl_len; i++) {
				struct ibv_gid_entry entry;
				int score;

				memset(&entry, 0, sizeof(entry));
				if (ibv_query_gid_ex(ctx, opts->ib_port, i,
						     &entry, 0) != 0)
					continue;
				if (memcmp(entry.gid.raw, target->raw,
					   sizeof(target->raw)) != 0)
					continue;

				dev_entries++;
				matching_entries++;
				print_match(dev_name, opts->ib_port, &entry,
					    false, false);

				score = gid_type_score(entry.gid_type);
				if (score <= 0)
					continue;
				if (!dev_best.found || score > dev_best.score) {
					dev_best.found = true;
					snprintf(dev_best.dev_name,
						 sizeof(dev_best.dev_name),
						 "%s", dev_name);
					dev_best.gid_index =
						(int)entry.gid_index;
					dev_best.gid_type = entry.gid_type;
					dev_best.score = score;
					dev_best.dev_order = d;
				}
			}
		}

		if (dev_entries > 0)
			matching_devices++;
		maybe_set_selected(&selected, &dev_best);
		ibv_close_device(ctx);
	}

	ibv_free_device_list(devs);

	if (!selected.found) {
		fprintf(stderr, "no llama.cpp-selectable RoCE GID matched target\n");
		return 2;
	}

	printf("llama_auto_selected dev=%s port=%u gid=%d type=%s\n",
	       selected.dev_name, opts->ib_port, selected.gid_index,
	       gid_type_name(selected.gid_type));

	if (opts->explicit_gid < 0 && matching_devices > 1) {
		printf("warning: %d devices and %d GID entries match target; llama.cpp will pick the first device order unless GGML_RDMA_DEV is set\n",
		       matching_devices, matching_entries);
		if (opts->fail_on_ambiguous)
			return 3;
	}

	return 0;
}

static bool nccl_gid_candidate(const struct options *opts,
			       const struct ibv_gid_entry *entry)
{
	return gid_is_valid_global(&entry->gid) &&
	       gid_addr_family(&entry->gid) == opts->nccl_addr_family &&
	       gid_roce_version(entry->gid_type) == opts->nccl_roce_version &&
	       nccl_gid_matches_range(opts, &entry->gid);
}

static int scan_nccl_devices(const struct options *opts)
{
	struct ibv_device **devs;
	int num_devs = 0;
	int selected_devices = 0;

	devs = ibv_get_device_list(&num_devs);
	if (!devs || num_devs == 0) {
		fprintf(stderr, "no libibverbs devices found\n");
		return 1;
	}

	print_nccl_config(opts);

	for (int d = 0; d < num_devs; d++) {
		const char *dev_name = ibv_get_device_name(devs[d]);
		struct ibv_context *ctx;
		struct ibv_port_attr port_attr;
		struct ibv_gid_entry selected = {0};
		bool found = false;

		if (opts->dev_filter && strcmp(opts->dev_filter, dev_name))
			continue;

		ctx = ibv_open_device(devs[d]);
		if (!ctx) {
			fprintf(stderr, "open %s: %s\n", dev_name,
				strerror(errno));
			continue;
		}

		memset(&port_attr, 0, sizeof(port_attr));
		if (ibv_query_port(ctx, opts->ib_port, &port_attr)) {
			fprintf(stderr, "query_port %s/%u: %s\n", dev_name,
				opts->ib_port, strerror(errno));
			ibv_close_device(ctx);
			continue;
		}

		if (opts->explicit_gid >= 0) {
			if (ibv_query_gid_ex(ctx, opts->ib_port,
					     (uint32_t)opts->explicit_gid,
					     &selected, 0) == 0)
				found = true;
		} else {
			for (uint32_t i = 0; i < port_attr.gid_tbl_len; i++) {
				struct ibv_gid_entry entry;

				memset(&entry, 0, sizeof(entry));
				if (ibv_query_gid_ex(ctx, opts->ib_port, i,
						     &entry, 0) != 0)
					continue;
				if (!nccl_gid_candidate(opts, &entry))
					continue;

				selected = entry;
				found = true;
				break;
			}
		}

		if (found) {
			selected_devices++;
			print_match(dev_name, opts->ib_port, &selected,
				    opts->explicit_gid >= 0, true);
			printf("nccl_auto_selected dev=%s port=%u gid=%u type=%s\n",
			       dev_name, opts->ib_port, selected.gid_index,
			       gid_type_name(selected.gid_type));
		} else {
			printf("nccl_no_match dev=%s port=%u\n", dev_name,
			       opts->ib_port);
		}

		ibv_close_device(ctx);
	}

	ibv_free_device_list(devs);

	if (!selected_devices) {
		fprintf(stderr, "no NCCL/RCCL-selectable GID matched config\n");
		return 2;
	}
	if (selected_devices > 1) {
		printf("warning: %d RDMA devices are NCCL/RCCL-selectable; vLLM may need NCCL_IB_HCA or a single logical device\n",
		       selected_devices);
		if (opts->fail_on_ambiguous)
			return 3;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct options opts = {0};
	union ibv_gid target;

	if (parse_options(argc, argv, &opts)) {
		usage(stderr, argv[0]);
		return 1;
	}

	if (opts.mode == MODE_NCCL)
		return scan_nccl_devices(&opts);

	if (opts.local_ip) {
		if (gid_from_ip(opts.local_ip, &target)) {
			fprintf(stderr, "invalid local IP address: %s\n",
				opts.local_ip);
			return 1;
		}
	} else if (connected_socket_gid(opts.connect_host, opts.connect_port,
				       opts.connect_timeout_sec,
				       &target)) {
		fprintf(stderr, "could not derive local GID from TCP connection to %s:%s\n",
			opts.connect_host, opts.connect_port);
		return 1;
	}

	print_target(&target);
	return scan_devices(&opts, &target);
}
