/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _USB4_RDMA_NHI_RAW_H
#define _USB4_RDMA_NHI_RAW_H

#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/thunderbolt.h>

bool usb4_rdma_nhi_raw_enabled(void);
int usb4_rdma_nhi_raw_tx(struct tb_ring *ring, struct ring_frame *frame);
int usb4_rdma_nhi_raw_tx_atomic(struct tb_ring *ring,
				struct ring_frame *frame);
int usb4_rdma_nhi_raw_tx_batch(struct tb_ring *ring, struct list_head *frames);
int usb4_rdma_nhi_raw_tx_batch_atomic(struct tb_ring *ring,
				      struct list_head *frames);
int usb4_rdma_nhi_raw_tx_batch_unlocked(struct tb_ring *ring,
					struct list_head *frames);
int usb4_rdma_nhi_raw_rx(struct tb_ring *ring, struct ring_frame *frame);
int usb4_rdma_nhi_raw_rx_batch(struct tb_ring *ring, struct list_head *frames);
int usb4_rdma_nhi_raw_rx_batch_atomic(struct tb_ring *ring,
				      struct list_head *frames);
int usb4_rdma_nhi_raw_rx_batch_unlocked(struct tb_ring *ring,
					struct list_head *frames);
struct ring_frame *usb4_rdma_nhi_raw_poll(struct tb_ring *ring);
unsigned int usb4_rdma_nhi_raw_poll_batch(struct tb_ring *ring,
					  struct list_head *frames,
					  unsigned int max);
unsigned int usb4_rdma_nhi_raw_poll_batch_unlocked(struct tb_ring *ring,
						   struct list_head *frames,
						   unsigned int max);
void usb4_rdma_nhi_raw_poll_complete(struct tb_ring *ring);
void usb4_rdma_nhi_raw_stats_show(struct seq_file *m);
void usb4_rdma_nhi_raw_ring_show(struct seq_file *m, const char *name,
				 struct tb_ring *ring);

#endif /* _USB4_RDMA_NHI_RAW_H */
