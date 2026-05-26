// SPDX-License-Identifier: MIT
/*
 * Tiny LD_PRELOAD tracer for RCCL/libibverbs debugging.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif

static int (*real_ibv_cmd_post_send)(struct ibv_qp *qp, struct ibv_send_wr *wr,
				     struct ibv_send_wr **bad_wr);
static int (*real_ibv_cmd_post_recv)(struct ibv_qp *qp, struct ibv_recv_wr *wr,
				     struct ibv_recv_wr **bad_wr);
static int (*real_ibv_cmd_poll_cq)(struct ibv_cq *cq, int num_entries,
				   struct ibv_wc *wc);
static struct ibv_mr *(*real_ibv_reg_mr)(struct ibv_pd *pd, void *addr,
					 size_t length, int access);
static struct ibv_mr *(*real_ibv_reg_mr_iova2)(struct ibv_pd *pd, void *addr,
					       size_t length, uint64_t iova,
					       unsigned int access);
static int (*real_ibv_dereg_mr)(struct ibv_mr *mr);

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static unsigned long post_send_count;
static unsigned long post_recv_count;
static unsigned long poll_cq_count;

static void resolve_syms(void)
{
	void *verbs = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);

	real_ibv_cmd_post_send = dlvsym(verbs ? verbs : RTLD_NEXT,
					"ibv_cmd_post_send",
					"IBVERBS_PRIVATE_59");
	real_ibv_cmd_post_recv = dlvsym(verbs ? verbs : RTLD_NEXT,
					"ibv_cmd_post_recv",
					"IBVERBS_PRIVATE_59");
	real_ibv_cmd_poll_cq = dlvsym(verbs ? verbs : RTLD_NEXT,
				      "ibv_cmd_poll_cq",
				      "IBVERBS_PRIVATE_59");
	real_ibv_reg_mr = dlsym(verbs ? verbs : RTLD_NEXT, "ibv_reg_mr");
	real_ibv_reg_mr_iova2 = dlsym(verbs ? verbs : RTLD_NEXT,
				      "ibv_reg_mr_iova2");
	real_ibv_dereg_mr = dlsym(verbs ? verbs : RTLD_NEXT, "ibv_dereg_mr");
	fprintf(stderr,
		"IBV_TRACE resolved post_send=%p post_recv=%p poll_cq=%p reg_mr=%p reg_mr_iova2=%p dereg_mr=%p\n",
		real_ibv_cmd_post_send, real_ibv_cmd_post_recv,
		real_ibv_cmd_poll_cq, real_ibv_reg_mr,
		real_ibv_reg_mr_iova2, real_ibv_dereg_mr);
	fflush(stderr);
}

static const char *op_name(int opcode)
{
	switch (opcode) {
	case IBV_WR_RDMA_WRITE:
		return "WRITE";
	case IBV_WR_RDMA_WRITE_WITH_IMM:
		return "WRITE_IMM";
	case IBV_WR_SEND:
		return "SEND";
	case IBV_WR_SEND_WITH_IMM:
		return "SEND_IMM";
	default:
		return "OTHER";
	}
}

int ibv_cmd_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
		      struct ibv_send_wr **bad_wr)
{
	unsigned long seq = __atomic_add_fetch(&post_send_count, 1,
					       __ATOMIC_RELAXED);
	int ret;

	pthread_once(&resolve_once, resolve_syms);
	if (seq <= 128) {
		for (struct ibv_send_wr *cur = wr; cur; cur = cur->next) {
			uint32_t len = cur->num_sge && cur->sg_list ?
				       cur->sg_list[0].length : 0;
			const unsigned char *p = cur->num_sge && cur->sg_list ?
						 (const unsigned char *)(uintptr_t)cur->sg_list[0].addr :
						 NULL;

			fprintf(stderr,
				"IBV_TRACE post_send seq=%lu tid=%ld qp=%u opcode=%s(%d) flags=0x%x wr_id=%lu len=%u imm=0x%x raddr=0x%lx rkey=0x%x\n",
				seq, (long)gettid(), qp ? qp->qp_num : 0,
				op_name(cur->opcode), cur->opcode,
				cur->send_flags, cur->wr_id, len,
				cur->imm_data, cur->wr.rdma.remote_addr,
				cur->wr.rdma.rkey);
			if (p && len == 64 && cur->opcode == IBV_WR_RDMA_WRITE) {
				uint64_t addr = *(const uint64_t *)(p + 0);
				uint64_t size = *(const uint64_t *)(p + 8);
				uint32_t rkey0 = *(const uint32_t *)(p + 16);
				uint32_t rkey1 = *(const uint32_t *)(p + 20);
				uint32_t rkey2 = *(const uint32_t *)(p + 24);
				uint32_t rkey3 = *(const uint32_t *)(p + 28);
				uint32_t nreqs = *(const uint32_t *)(p + 32);
				uint32_t tag = *(const uint32_t *)(p + 36);
				uint64_t idx = *(const uint64_t *)(p + 40);

				fprintf(stderr,
					"IBV_TRACE fifo_payload addr=0x%lx size=%lu rkeys=%x,%x,%x,%x nreqs=%u tag=0x%x idx=%lu\n",
					addr, size, rkey0, rkey1, rkey2,
					rkey3, nreqs, tag, idx);
			}
		}
	}
	if (!real_ibv_cmd_post_send)
		return ENOSYS;
	ret = real_ibv_cmd_post_send(qp, wr, bad_wr);
	if (ret || seq <= 128)
		fprintf(stderr, "IBV_TRACE post_send_ret seq=%lu ret=%d\n",
			seq, ret);
	return ret;
}

struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
			  int access)
{
	struct ibv_mr *mr;

	pthread_once(&resolve_once, resolve_syms);
	if (!real_ibv_reg_mr) {
		errno = ENOSYS;
		return NULL;
	}
	mr = real_ibv_reg_mr(pd, addr, length, access);
	fprintf(stderr,
		"IBV_TRACE reg_mr addr=%p length=%zu access=0x%x -> mr=%p lkey=0x%x rkey=0x%x\n",
		addr, length, access, mr, mr ? mr->lkey : 0,
		mr ? mr->rkey : 0);
	return mr;
}

struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t length,
				uint64_t iova, unsigned int access)
{
	struct ibv_mr *mr;

	pthread_once(&resolve_once, resolve_syms);
	if (!real_ibv_reg_mr_iova2) {
		errno = ENOSYS;
		return NULL;
	}
	mr = real_ibv_reg_mr_iova2(pd, addr, length, iova, access);
	fprintf(stderr,
		"IBV_TRACE reg_mr_iova2 addr=%p length=%zu iova=0x%lx access=0x%x -> mr=%p lkey=0x%x rkey=0x%x\n",
		addr, length, (unsigned long)iova, access, mr,
		mr ? mr->lkey : 0, mr ? mr->rkey : 0);
	return mr;
}

int ibv_dereg_mr(struct ibv_mr *mr)
{
	int ret;

	pthread_once(&resolve_once, resolve_syms);
	fprintf(stderr,
		"IBV_TRACE dereg_mr mr=%p lkey=0x%x rkey=0x%x\n",
		mr, mr ? mr->lkey : 0, mr ? mr->rkey : 0);
	if (!real_ibv_dereg_mr)
		return ENOSYS;
	ret = real_ibv_dereg_mr(mr);
	fprintf(stderr, "IBV_TRACE dereg_mr_ret ret=%d\n", ret);
	return ret;
}

int ibv_cmd_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
		      struct ibv_recv_wr **bad_wr)
{
	unsigned long seq = __atomic_add_fetch(&post_recv_count, 1,
					       __ATOMIC_RELAXED);
	int ret;

	pthread_once(&resolve_once, resolve_syms);
	if (seq <= 128) {
		for (struct ibv_recv_wr *cur = wr; cur; cur = cur->next) {
			fprintf(stderr,
				"IBV_TRACE post_recv seq=%lu tid=%ld qp=%u wr_id=%lu num_sge=%d\n",
				seq, (long)gettid(), qp ? qp->qp_num : 0,
				cur->wr_id, cur->num_sge);
		}
	}
	if (!real_ibv_cmd_post_recv)
		return ENOSYS;
	ret = real_ibv_cmd_post_recv(qp, wr, bad_wr);
	if (ret || seq <= 128)
		fprintf(stderr, "IBV_TRACE post_recv_ret seq=%lu ret=%d\n",
			seq, ret);
	return ret;
}

int ibv_cmd_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
	unsigned long seq = __atomic_add_fetch(&poll_cq_count, 1,
					       __ATOMIC_RELAXED);
	int ret;

	pthread_once(&resolve_once, resolve_syms);
	if (!real_ibv_cmd_poll_cq)
		return -1;
	ret = real_ibv_cmd_poll_cq(cq, num_entries, wc);
	if (ret || seq <= 16) {
		fprintf(stderr,
			"IBV_TRACE poll_cq seq=%lu tid=%ld n=%d ret=%d\n",
			seq, (long)gettid(), num_entries, ret);
		for (int i = 0; i < ret; i++)
			fprintf(stderr,
				"IBV_TRACE wc seq=%lu idx=%d wr_id=%lu status=%d opcode=%d byte_len=%u imm=0x%x qp=%u\n",
				seq, i, wc[i].wr_id, wc[i].status,
				wc[i].opcode, wc[i].byte_len,
				wc[i].imm_data, wc[i].qp_num);
	}
	return ret;
}
