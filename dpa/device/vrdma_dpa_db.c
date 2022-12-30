/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include <common/flexio_common.h>
#include <libflexio-libc/string.h>
#include <libflexio-libc/stdio.h>
#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include "../vrdma_dpa_common.h"
#include "vrdma_dpa_dev_com.h"
#include "vrdma_dpa_cq.h"

#define DPA_DEBUG
// #define DPA_DEBUG_DETAIL

static int get_next_qp_swqe_index(uint32_t pi, uint32_t depth)
{
	return (pi % depth);
}

static flexio_dev_status_t swqe_seg_ctrl_set_rdmaw_immd(union flexio_dev_sqe_seg *swqe, uint32_t sq_pi,
						 uint32_t sq_number, uint32_t ce, uint32_t imm)
{
	uint32_t ds_count;
	uint32_t opcode;
	uint32_t mod;

	/* default for common case */
	mod = 0;
	opcode = MLX5_CTRL_SEG_OPCODE_RDMA_WRITE_WITH_IMMEDIATE;
	ds_count = 3;
	/* Fill out 1-st segment (Control) */
	swqe->ctrl.idx_opcode = cpu_to_be32((mod << 24) | ((sq_pi & 0xffff) << 8) | opcode);
	swqe->ctrl.qpn_ds = cpu_to_be32((sq_number << 8) | ds_count);
	swqe->ctrl.signature_fm_ce_se = cpu_to_be32(ce << 2);
	swqe->ctrl.general_id = cpu_to_be32(imm);

	return FLEXIO_DEV_STATUS_SUCCESS;
}

static void vrdma_dpa_wr_pi_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					uint32_t remote_key,
					uint64_t remote_addr,
					uint32_t local_key,
					uint64_t local_addr,
					uint32_t size,
					uint16_t wr_pi)
{
	int swqe_index;
	union flexio_dev_sqe_seg *swqe;

	swqe_index = get_next_qp_swqe_index(ehctx->dma_qp.hw_qp_sq_pi,
					    ehctx->dma_qp.hw_qp_depth);
	swqe = (union flexio_dev_sqe_seg *)
		(ehctx->dma_qp.qp_sq_buff + (swqe_index * 64));

	/* Fill out 1-st segment (Control) rdma write immediately*/
	swqe_seg_ctrl_set_rdmaw_immd(swqe, ehctx->dma_qp.hw_qp_sq_pi,
					ehctx->dma_qp.qp_num,
					MLX5_CTRL_SEG_CE_CQE_ALWAYS,
					ehctx->vq_index<<16 | wr_pi);

	/* Fill out 2-nd segment (RDMA) */
	swqe++;
	flexio_dev_swqe_seg_rdma_set(swqe, remote_key,
					remote_addr);

	/* Fill out 3-rd segment (local Data) */
	swqe++;
	flexio_dev_swqe_seg_data_set(swqe, size,
					local_key,
					local_addr);
	ehctx->dma_qp.hw_qp_sq_pi++; /*pi is for each wqebb*/
}
// #endif

static bool vrdma_dpa_rq_wr_pi_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					uint16_t rq_start_pi, uint16_t rq_end_pi,
					uint16_t rq_pi)
{
	uint32_t remote_key, local_key, size;
	uint64_t remote_addr, local_addr;
	uint16_t index, wqebb_size;

	/*notice: now both host and arm wqebb(wr) has same size and count*/
	index = rq_start_pi;
	wqebb_size = ehctx->dma_qp.host_vq_ctx.rq_wqebb_size;

	local_key  = ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey;
	local_addr = ehctx->dma_qp.host_vq_ctx.rq_wqe_buff_pa +
		      wqebb_size * index;

	remote_key   = ehctx->dma_qp.arm_vq_ctx.rq_lkey;
	remote_addr  = ehctx->dma_qp.arm_vq_ctx.rq_buff_addr +
		      wqebb_size * index;

	size = (rq_end_pi - rq_start_pi) * wqebb_size;
	vrdma_dpa_wr_pi_fetch(ehctx, remote_key, remote_addr, local_key,
				local_addr, size, rq_pi);
#ifdef DPA_DEBUG_DETAIL
	printf("---naliu rq: index %#x, wqebb_size %#x, size %#x, remote_key %#x, remote_addr %#lx,"
			"local_key %#x, local_addr %#lx\n",
			index, wqebb_size, size, remote_key, remote_addr, local_key, local_addr);
#endif
	return true;
}

static bool vrdma_dpa_sq_wr_pi_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					uint16_t sq_start_pi, uint16_t sq_end_pi,
					uint16_t sq_pi)
{
	uint32_t remote_key, local_key, size;
	uint64_t remote_addr, local_addr;
	uint16_t index, wqebb_size;

	/*notice: now both host and arm wqebb(wr) has same size and count*/
	index = sq_start_pi;
	wqebb_size = ehctx->dma_qp.host_vq_ctx.sq_wqebb_size;

	local_key  = ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey;
	local_addr = ehctx->dma_qp.host_vq_ctx.sq_wqe_buff_pa +
		      wqebb_size * index;

	remote_key   = ehctx->dma_qp.arm_vq_ctx.sq_lkey;
	remote_addr  = ehctx->dma_qp.arm_vq_ctx.sq_buff_addr +
		      wqebb_size * index;

	size = (sq_end_pi - sq_start_pi) * wqebb_size;
	if (size > 0x400) {
		ehctx->count[4] ++;
	}
	vrdma_dpa_wr_pi_fetch(ehctx, remote_key, remote_addr, local_key,
				local_addr, size, sq_pi);
#ifdef DPA_DEBUG_DETAIL
	printf("---naliu sq: sq_start_pi %#x, sq_end_pi %#x, sq_pi %#x, wqebb_size %#x, size %#x, remote_key %#x, remote_addr %#lx,"
			"local_key %#x, local_addr %#lx\n",
			sq_start_pi, sq_end_pi, sq_pi, wqebb_size, size, remote_key, remote_addr, local_key, local_addr);
#endif	
	return true;
}

__FLEXIO_ENTRY_POINT_START
flexio_dev_event_handler_t vrdma_db_handler;
void vrdma_db_handler(flexio_uintptr_t thread_arg)
{
	struct vrdma_dpa_event_handler_ctx *ehctx;
	struct flexio_dev_thread_ctx *dtctx;
	uint16_t rq_last_fetch_start;
	uint16_t sq_last_fetch_start;
	uint16_t rq_last_fetch_end = 0;
	uint16_t sq_last_fetch_end = 0;
	uint16_t rq_pi = 0 , sq_pi = 0;
	bool has_wqe = false;

	flexio_dev_get_thread_ctx(&dtctx);
	ehctx = (struct vrdma_dpa_event_handler_ctx *)thread_arg;
//	printf("%s: --------virtq status %d.\n", __func__, ehctx->dma_qp.state);
	if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
		printf("%s: ------virtq status %d is not READY.\n", __func__, ehctx->dma_qp.state);
		goto out;
	}
	flexio_dev_outbox_config(dtctx, ehctx->emu_outbox);
	flexio_dev_window_mkey_config(dtctx,
				      ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey);
	flexio_dev_window_ptr_acquire(dtctx, 0,
		(flexio_uintptr_t **)&ehctx->window_base_addr);

	// printf("---naliu vq_idx %d, emu_outbox %d, emu_crossing_mkey %d\n",
		// ehctx->vq_index, ehctx->emu_outbox, ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey);
	// printf("---naliu window_base_addr %#x\n", ehctx->window_base_addr);
#ifdef DPA_DEBUG
	printf("---naliu rq_wqe_buff_pa %#lx, rq_pi_paddr %#lx, rq_wqebb_cnt %#x,"
			"rq_wqebb_size %#x, sq_wqe_buff_pa %#lx, sq_pi_paddr %#lx,"
			"sq_wqebb_cnt %#x, sq_wqebb_size %#lx, emu_crossing_mkey %#x,"
			"sf_crossing_mkey %#x\n",
			ehctx->dma_qp.host_vq_ctx.rq_wqe_buff_pa, ehctx->dma_qp.host_vq_ctx.rq_pi_paddr,
			ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt, ehctx->dma_qp.host_vq_ctx.rq_wqebb_size,
			ehctx->dma_qp.host_vq_ctx.sq_wqe_buff_pa, ehctx->dma_qp.host_vq_ctx.sq_pi_paddr,
			ehctx->dma_qp.host_vq_ctx.sq_wqebb_cnt, ehctx->dma_qp.host_vq_ctx.sq_wqebb_size,
			ehctx->dma_qp.host_vq_ctx.emu_crossing_mkey, ehctx->dma_qp.host_vq_ctx.sf_crossing_mkey);
#endif
	rq_last_fetch_start = ehctx->rq_last_fetch_start;
	sq_last_fetch_start = ehctx->sq_last_fetch_start;
	/*fetch rq_pi*/
	rq_pi = *(uint16_t*)(ehctx->window_base_addr + 
				ehctx->dma_qp.host_vq_ctx.rq_pi_paddr);
	sq_pi = *(uint16_t*)(ehctx->window_base_addr + 
				ehctx->dma_qp.host_vq_ctx.sq_pi_paddr);

	rq_last_fetch_end = rq_pi % ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;
	sq_last_fetch_end = sq_pi % ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;

	// while ((rq_last_fetch_start != rq_last_fetch_end) || 
	// 	(sq_last_fetch_start != sq_last_fetch_end))
	while (1)
	{
		if (rq_last_fetch_start < rq_last_fetch_end) {
			has_wqe = vrdma_dpa_rq_wr_pi_fetch(ehctx, rq_last_fetch_start, rq_last_fetch_end, rq_pi);
		} else if (rq_last_fetch_start > rq_last_fetch_end) {
			has_wqe = vrdma_dpa_rq_wr_pi_fetch(ehctx,
						rq_last_fetch_start,
						ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt, rq_pi - rq_last_fetch_end);
			has_wqe = vrdma_dpa_rq_wr_pi_fetch(ehctx, 0, rq_last_fetch_end, rq_pi);
		}

		if (sq_last_fetch_start < sq_last_fetch_end) {
			has_wqe = vrdma_dpa_sq_wr_pi_fetch(ehctx, sq_last_fetch_start, sq_last_fetch_end, sq_pi);
			ehctx->count[0] ++;
		} else if (sq_last_fetch_start > sq_last_fetch_end) {
			has_wqe = vrdma_dpa_sq_wr_pi_fetch(ehctx,
						sq_last_fetch_start,
						ehctx->dma_qp.host_vq_ctx.sq_wqebb_cnt, sq_pi - sq_last_fetch_end);
			ehctx->count[1] ++;
			if (sq_last_fetch_end != 0) {
				ehctx->count[2] ++;
				sq_last_fetch_start = vrdma_dpa_sq_wr_pi_fetch(ehctx, 0, sq_last_fetch_end, sq_pi);
			} else {
				ehctx->count[3] ++;
			}
		}

		if (has_wqe) {
			rq_last_fetch_start = rq_last_fetch_end;
			sq_last_fetch_start = sq_last_fetch_end;

			flexio_dev_dbr_sq_set_pi((uint32_t *)
					ehctx->dma_qp.dbr_daddr + 1,
					ehctx->dma_qp.hw_qp_sq_pi);
			flexio_dev_qp_sq_ring_db(dtctx, ehctx->dma_qp.hw_qp_sq_pi,
						ehctx->dma_qp.qp_num);
		}

		if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
			printf("%s: Now virtq status is not READY.\n", __func__);
			goto out;
		}
		/*fetch rq_pi*/
		rq_pi = *(uint16_t*)(ehctx->window_base_addr + 
					ehctx->dma_qp.host_vq_ctx.rq_pi_paddr);
		sq_pi = *(uint16_t*)(ehctx->window_base_addr + 
					ehctx->dma_qp.host_vq_ctx.sq_pi_paddr);
		rq_last_fetch_end = rq_pi % ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;
		sq_last_fetch_end = sq_pi % ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;
	}
	ehctx->rq_last_fetch_start = rq_last_fetch_start;
	ehctx->sq_last_fetch_start = sq_last_fetch_start;
	
out:
	vrdma_dpa_db_cq_incr(&ehctx->guest_db_cq_ctx);
	flexio_dev_dbr_cq_set_ci(ehctx->guest_db_cq_ctx.dbr,
				 ehctx->guest_db_cq_ctx.ci);
	flexio_dev_db_ctx_arm(dtctx, ehctx->guest_db_cq_ctx.cqn,
			      ehctx->emu_db_to_cq_id);
	flexio_dev_cq_arm(dtctx, ehctx->guest_db_cq_ctx.ci,
			  ehctx->guest_db_cq_ctx.cqn);
#ifdef DPA_DEBUG
	printf("\n------naliu count[0] %d, count[1] %d, count[2] %d, count[3] %d, count[4] %d",
		ehctx->count[0], ehctx->count[1], ehctx->count[2], ehctx->count[3], ehctx->count[4]);
	printf("\n------naliu rq_pi %d, sq_pi %d\n", rq_pi, sq_pi);
	printf("\n------naliu rq_last_fetch_end %d, sq_last_fetch_end %d\n", rq_last_fetch_end, sq_last_fetch_end);
	printf("\n------naliu dma_qp.hw_qp_sq_pi %d\n", ehctx->dma_qp.hw_qp_sq_pi);
	printf("\n------naliu vrdma_db_handler done. cqn: %#x, emu_db_to_cq_id %d, guest_db_cq_ctx.ci %d\n",
		ehctx->guest_db_cq_ctx.cqn, ehctx->emu_db_to_cq_id, ehctx->guest_db_cq_ctx.ci);
#endif
	flexio_dev_return();
}
__FLEXIO_ENTRY_POINT_END
