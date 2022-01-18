/*
 * Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */
#include <stdarg.h>

#include "dpa.h"
#include "snap_dma.h"
#include "snap_dma_internal.h"

/**
 * dpa_dma_ep_init() - initialize dma_q endpoint on DPA
 * @tcb: thread control block
 * @q: dpa endpoint address in dpa memory
 *
 * The function takes endpoint that was created with snap_dma_ep_create()
 * and prepares it for use on DPA
 */
int dpa_dma_ep_init(struct snap_dpa_tcb *tcb, struct snap_dma_q *q)
{
	uint32_t sq_wqe_cnt = q->sw_qp.dv_qp.hw_qp.sq.wqe_cnt;
	uint32_t rq_wqe_cnt = q->sw_qp.dv_qp.hw_qp.rq.wqe_cnt;
	int i;

	/* This is an example how to use thread allocator. It is definitely
	 * possible to preinit buffer on DPU and copy to device memory as
	 * we are doing in snap_cq.
	 */
	/* If qp has non zero rx post receives */
	if (rq_wqe_cnt) {
		q->sw_qp.rx_buf = dpa_thread_alloc(tcb, 2 * rq_wqe_cnt * q->rx_elem_size);
		for (i = 0; i < 2 * rq_wqe_cnt; i++) {
			snap_dv_post_recv(&q->sw_qp.dv_qp,
					  q->sw_qp.rx_buf + i * q->rx_elem_size,
					  q->rx_elem_size,
					  snap_dma_q_dpa_mkey(q));
			snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
		}
	}

	/* setup completion memory */
	if (sq_wqe_cnt) {
		q->sw_qp.dv_qp.comps = dpa_thread_alloc(tcb, sq_wqe_cnt * sizeof(struct snap_dv_dma_completion));
		memset(q->sw_qp.dv_qp.comps, 0, sq_wqe_cnt * sizeof(struct snap_dv_dma_completion));
	}

	/* Setup a correct ops pointer */
	q->ops = &dv_ops;
	return 0;
}

struct snap_dma_q *dpa_dma_ep_cmd_copy(struct snap_dpa_tcb *tcb, struct snap_dpa_cmd *cmd)
{
	struct snap_dma_ep_copy_cmd *ep_cmd = (struct snap_dma_ep_copy_cmd *)cmd;
	struct snap_dma_q *q;

	q = dpa_thread_alloc(tcb, sizeof(*q));
	memcpy(q, &ep_cmd->q, sizeof(*q));
	dpa_dma_ep_init(tcb, q);
	return q;
}

/**
 * dpa_thread_alloc() - allocate memory on thread heap
 * @tcb: thread control block
 * @size: amount of memory to allocate
 *
 * The function implements sbrk() like memory allocator. It reserves requested
 * amount of memory on per thread heap.
 *
 * The function never returns NULL
 *
 * TODO: error checking
 *
 * Return:
 * pointer to the reserved memory, fatal event on failure
 */
void *dpa_thread_alloc(struct snap_dpa_tcb *tcb, size_t size)
{
	void *data_add = (void *) tcb->data_address + tcb->data_used;

	//size is rounded up to cache line (64 bytes)
	if (size % DPA_CACHE_LINE_BYTES)
		tcb->data_used += size + (DPA_CACHE_LINE_BYTES - (size % DPA_CACHE_LINE_BYTES));
	else
		tcb->data_used += size;

	if (tcb->data_used > SNAP_DPA_THREAD_HEAP_SIZE)
		dpa_print_one_arg("thread alloc: OOM: used more than ", SNAP_DPA_THREAD_HEAP_SIZE);

	dpa_print_one_arg("thread alloc: addr ", (uint64_t)data_add);
	dpa_print_one_arg("thread alloc: used ", (uint64_t)tcb->data_used);
	return data_add;
}

/**
 * dpa_thread_free() - free memory on the per thread heap
 * @tcb: thread control block
 * @addr: address of memory block to free
 *
 * The function does nothing at the moment. In the future it may
 * set heap top to the @addr.
 */
void dpa_thread_free(struct snap_dpa_tcb *tcb, void *addr)
{
}

/* TODO: per dpa process/thread logging instead of dumping to simx */
static int do_print(const char *format, va_list ap)
{
	char str[SNAP_DPA_PRINT_BUF_LEN];
	int ret;

	ret = vsnprintf(str, sizeof(str), format, ap);
	dpa_print_string(str);
	return ret;
}

/* override builtin printfs */
int printf(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = do_print(format, ap);
	va_end(ap);
	return ret;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = do_print(format, ap);
	va_end(ap);
	return ret;
}