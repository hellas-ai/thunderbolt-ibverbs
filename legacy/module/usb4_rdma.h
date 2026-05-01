/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _USB4_RDMA_H
#define _USB4_RDMA_H

#include <linux/debugfs.h>
#include <linux/types.h>

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

/* data.c — per-peer ring management + wire protocol. */
struct tb_service;
struct device;
struct page;
struct u4_wire_hdr;
typedef int (*usb4_rdma_data_fill_fn)(void *dst, u32 length, void *ctx);
typedef void (*usb4_rdma_data_done_fn)(void *ctx);
typedef int (*usb4_rdma_data_next_page_fn)(void *ctx, struct page **page,
					   u32 *page_off, u32 *length,
					   dma_addr_t *dma_addr,
					   bool *dma_mapped,
					   usb4_rdma_data_done_fn *done,
					   void **done_ctx);
int  usb4_rdma_data_init(struct dentry *parent_dir);
void usb4_rdma_data_exit(void);
/* Returns 0 once peer login has been queued, or a negative errno on
 * synchronous setup failure. The async login worker emits peer events. */
int  usb4_rdma_data_attach_peer(struct tb_service *svc);
bool usb4_rdma_data_detach_peer(struct tb_service *svc);
int  usb4_rdma_data_send(u8 opcode, u32 src_qp, u32 dest_qp, u32 psn,
			 u8 flags, __be32 imm_data, u64 remote_addr, u32 rkey,
			 usb4_rdma_data_fill_fn fill, void *fill_ctx,
			 u32 length);
int  usb4_rdma_data_send_page(u8 opcode, u32 src_qp, u32 dest_qp, u32 psn,
			      u8 flags, __be32 imm_data, u64 remote_addr,
			      u32 rkey, struct page *page, u32 page_off,
			      u32 length, usb4_rdma_data_done_fn done,
			      void *done_ctx);
int  usb4_rdma_data_send_page_stream(u8 opcode, u32 src_qp, u32 dest_qp,
				     u32 psn, u8 flags, __be32 imm_data,
				     u64 remote_addr, u32 rkey,
				     u32 total_length,
				     usb4_rdma_data_next_page_fn next,
				     void *next_ctx);
int  usb4_rdma_data_register_qp(u32 qp_num, void *qp);
void usb4_rdma_data_unregister_qp(u32 qp_num);
void usb4_rdma_data_set_rx_handler(void (*h)(void *qp,
					     const struct u4_wire_hdr *hdr,
					     const void *payload, u32 length));
bool usb4_rdma_data_peer_attached(void);
int usb4_rdma_data_active_lane_count(void);
struct device *usb4_rdma_data_dma_dev_get(void);
void usb4_rdma_data_dma_dev_put(struct device *dev);

#endif /* _USB4_RDMA_H */
