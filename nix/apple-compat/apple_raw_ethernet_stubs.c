#include <stdint.h>

/*
 * Raw Ethernet mode is disabled in the Darwin perftest build. These helpers
 * are still referenced by common perftest objects, so keep dyld satisfied for
 * the unreachable RawEth path without pretending to support flow steering.
 */

struct ibv_flow_attr;
struct memory_ctx;
struct perftest_parameters;
struct pingpong_context;

void print_ethernet_header(void *p_ethernet_header,
			   struct perftest_parameters *user_param,
			   struct memory_ctx *memory)
{
	(void)p_ethernet_header;
	(void)user_param;
	(void)memory;
}

void print_ethernet_vlan_header(void *p_ethernet_header,
				struct perftest_parameters *user_param,
				struct memory_ctx *memory)
{
	(void)p_ethernet_header;
	(void)user_param;
	(void)memory;
}

int set_up_fs_rules(struct ibv_flow_attr **flow_rules,
		    struct pingpong_context *ctx,
		    struct perftest_parameters *user_param,
		    uint64_t allocated_flows)
{
	(void)flow_rules;
	(void)ctx;
	(void)user_param;
	(void)allocated_flows;
	return 1;
}
