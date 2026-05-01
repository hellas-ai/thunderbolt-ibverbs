/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _USB4_RDMA_H
#define _USB4_RDMA_H

#include <linux/debugfs.h>

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

#endif /* _USB4_RDMA_H */
