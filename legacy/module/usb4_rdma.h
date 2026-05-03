/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _USB4_RDMA_H
#define _USB4_RDMA_H

#include <linux/debugfs.h>
#include <linux/dma-direction.h>
#include <linux/types.h>

struct u4_data_peer;

/* bar.c — read-only BAR0 explorer for USB4 host routers. */
int  usb4_rdma_pci_init(struct dentry *parent_dir);
void usb4_rdma_pci_exit(void);

/* loadtest.c — multi-ring xdomain throughput probe. */
int  usb4_rdma_loadtest_init(struct dentry *parent_dir);
void usb4_rdma_loadtest_exit(void);

/* ibdev.c — soft-RDMA ib_device skeleton. */
int  usb4_rdma_ibdev_init(void);
void usb4_rdma_ibdev_exit(void);
void usb4_rdma_ibdev_peer_event(bool joined);
void usb4_rdma_ibdev_rail_event(struct u4_data_peer *rail, bool joined);

/* data.c — per-peer ring management + wire protocol. */
struct tb_service;
struct device;
struct page;
struct u4_wire_hdr;
typedef int (*usb4_rdma_data_fill_fn)(void *dst, u32 length, void *ctx);
typedef void (*usb4_rdma_data_done_fn)(void *ctx);
typedef int (*usb4_rdma_data_dma_resolve_fn)(
	void *ctx, struct device *dma_dev, u32 page_idx, u32 page_off,
	u32 length, enum dma_data_direction dir, dma_addr_t *dma_addr);
typedef int (*usb4_rdma_data_next_page_fn)(void *ctx, struct page **page,
					   u32 *page_idx, u32 *page_off,
					   u32 *length,
					   usb4_rdma_data_dma_resolve_fn *resolve,
					   void **resolve_ctx,
					   usb4_rdma_data_done_fn *done,
					   void **done_ctx);
typedef int (*usb4_rdma_data_rx_next_page_fn)(void *ctx, struct page **page,
					      u32 *page_off, u32 *length,
					      usb4_rdma_data_done_fn *done,
					      void **done_ctx);
typedef void (*usb4_rdma_data_rx_finish_fn)(void *ctx);
typedef int (*usb4_rdma_data_rx_zcopy_prepare_fn)(
	void *qp, const struct u4_wire_hdr *hdr, u32 total_length,
	usb4_rdma_data_rx_next_page_fn *next, void **next_ctx,
	usb4_rdma_data_rx_finish_fn *finish);
int  usb4_rdma_data_init(struct dentry *parent_dir);
void usb4_rdma_data_exit(void);
/* Returns 0 once peer login has been queued, or a negative errno on
 * synchronous setup failure. The async login worker emits peer events. */
int  usb4_rdma_data_attach_peer(struct tb_service *svc);
bool usb4_rdma_data_detach_peer(struct tb_service *svc);
bool usb4_rdma_data_rail_get(struct u4_data_peer *rail);
void usb4_rdma_data_rail_put(struct u4_data_peer *rail);
int usb4_rdma_data_rail_index(struct u4_data_peer *rail);
int  usb4_rdma_data_send(struct u4_data_peer *rail, u8 opcode,
			 u32 src_qp, u32 dest_qp, u32 psn,
			 u8 flags, __be32 imm_data, u64 remote_addr, u32 rkey,
			 usb4_rdma_data_fill_fn fill, void *fill_ctx,
			 u32 length);
int  usb4_rdma_data_send_ack_atomic(struct u4_data_peer *rail,
				    u32 src_qp, u32 dest_qp, u32 psn,
				    __be32 status);
int  usb4_rdma_data_send_ack_try(struct u4_data_peer *rail,
				 u32 src_qp, u32 dest_qp, u32 psn,
				 __be32 status);
int  usb4_rdma_data_send_page(struct u4_data_peer *rail, u8 opcode,
			      u32 src_qp, u32 dest_qp, u32 psn,
			      u8 flags, __be32 imm_data, u64 remote_addr,
			      u32 rkey, struct page *page, u32 page_off,
			      u32 length, usb4_rdma_data_done_fn done,
			      void *done_ctx);
int  usb4_rdma_data_send_page_stream(struct u4_data_peer *rail, u8 opcode,
				     u32 src_qp, u32 dest_qp,
				     u32 psn, u8 flags, __be32 imm_data,
				     u64 remote_addr, u32 rkey,
				     u32 total_length,
				     usb4_rdma_data_next_page_fn next,
				     void *next_ctx);
int  usb4_rdma_data_register_qp(u32 qp_num, void *qp,
				struct u4_data_peer *rail);
void usb4_rdma_data_unregister_qp(u32 qp_num, struct u4_data_peer *rail);
void usb4_rdma_data_set_rx_handler(void (*h)(void *qp,
					     const struct u4_wire_hdr *hdr,
					     const void *payload, u32 length));
void usb4_rdma_data_set_rx_zcopy_prepare(
	usb4_rdma_data_rx_zcopy_prepare_fn prepare);
bool usb4_rdma_data_peer_attached(void);
int usb4_rdma_data_poll_rx(void);
int usb4_rdma_data_active_lane_count(void);
struct device *usb4_rdma_data_dma_dev_get(void);
void usb4_rdma_data_dma_dev_put(struct device *dev);

#endif /* _USB4_RDMA_H */
