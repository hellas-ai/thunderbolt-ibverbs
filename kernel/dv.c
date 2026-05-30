// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

/*
 * UVERBS_MODULE_NAME must be defined before including
 * rdma/uverbs_named_ioctl.h; the macro is used in the DECLARE_UVERBS_*
 * helpers below to namespace generated symbols.
 */
#define UVERBS_MODULE_NAME tbv

#include <linux/build_bug.h>
#include <linux/types.h>
#include <rdma/ib_verbs.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_named_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>

#include "../userspace/usb4_rdma/usb4_rdma_dv.h"
#include "tbv.h"

/*
 * USB4 RDMA Direct Verbs (DV) ABI surface.
 *
 * The DV ABI is the software-RNIC contract used by the upcoming GDA path: the
 * GPU produces work queue entries into host-visible coherent memory and the
 * kernel poll worker consumes them. See userspace/usb4_rdma/usb4_rdma_dv.h
 * for the full design, including memory-ordering rules and the generation
 * protocol.
 *
 * This file implements the first piece of that surface: QUERY_CAPS. It is
 * intentionally the only method wired today. CREATE_QUEUE, DESTROY_QUEUE,
 * and KICK will land in subsequent commits behind the capability bits in
 * struct usb4_rdma_dv_query_caps_resp::caps.
 */

static int UVERBS_HANDLER(USB4_RDMA_DV_METHOD_QUERY_CAPS)(
	struct uverbs_attr_bundle *attrs)
{
	struct usb4_rdma_dv_query_caps_resp resp = {
		.abi_version = USB4_RDMA_DV_ABI_VERSION,
		/*
		 * No transport opcodes are wired through the DV consumer yet,
		 * so we advertise an empty capability bitmap. Each subsequent
		 * commit that enables a real WQE opcode will OR the matching
		 * USB4_RDMA_DV_CAP_* bit in here so userspace can detect what
		 * the kernel will actually consume.
		 */
		.caps = 0,
		.max_sq_entries = USB4_RDMA_DV_MAX_SQ_ENTRIES,
		.max_cq_entries = USB4_RDMA_DV_MAX_CQ_ENTRIES,
		.default_sq_entries = USB4_RDMA_DV_DEFAULT_SQ_ENTRIES,
		.default_cq_entries = USB4_RDMA_DV_DEFAULT_CQ_ENTRIES,
		.wqe_size = USB4_RDMA_DV_WQE_SIZE,
		.cqe_size = USB4_RDMA_DV_CQE_SIZE,
		.doorbell_record_size = USB4_RDMA_DV_DOORBELL_RECORD_SIZE,
		.doorbell_page_size = USB4_RDMA_DV_DOORBELL_PAGE_SIZE,
		.tail_index_bits = USB4_RDMA_DV_TAIL_INDEX_BITS,
		.tail_generation_bits = USB4_RDMA_DV_TAIL_GENERATION_BITS,
	};
	struct ib_ucontext *ib_uctx;

	/*
	 * Compile-time guards: the ABI sizes that userspace receives in the
	 * QUERY_CAPS response are the same sizes the future producer/consumer
	 * code will assume for the on-the-wire-via-memory structs. If a
	 * struct field is added without updating the corresponding _SIZE
	 * constant (and bumping the ABI version), userspace and kernel will
	 * silently disagree about the layout. Catch that at build time.
	 */
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_wqe) !=
		     USB4_RDMA_DV_WQE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_cqe) !=
		     USB4_RDMA_DV_CQE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_doorbell_producer_line) !=
		     USB4_RDMA_DV_DOORBELL_LINE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_doorbell_consumer_line) !=
		     USB4_RDMA_DV_DOORBELL_LINE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_doorbell) !=
		     USB4_RDMA_DV_DOORBELL_RECORD_SIZE);

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);
	if (!tbv_ibdev_state(ib_uctx->device))
		return -ENODEV;

	return uverbs_copy_to(attrs, USB4_RDMA_DV_ATTR_QUERY_CAPS_RESP,
			      &resp, sizeof(resp));
}

DECLARE_UVERBS_NAMED_METHOD(
	USB4_RDMA_DV_METHOD_QUERY_CAPS,
	UVERBS_ATTR_PTR_OUT(
		USB4_RDMA_DV_ATTR_QUERY_CAPS_RESP,
		UVERBS_ATTR_STRUCT(struct usb4_rdma_dv_query_caps_resp,
				   reserved),
		UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(
	USB4_RDMA_DV_OBJECT_DEVICE,
	&UVERBS_METHOD(USB4_RDMA_DV_METHOD_QUERY_CAPS));

const struct uapi_definition tbv_uapi_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(USB4_RDMA_DV_OBJECT_DEVICE),
	{},
};
