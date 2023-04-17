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

#ifndef __VRDMA_DPA_COMMON_H__
#define __VRDMA_DPA_COMMON_H__

#include <string.h>
#include <stdint.h>
#include <common/flexio_common_structs.h>
//#include <libflexio/flexio.h>
#include <libflexio-dev/flexio_dev.h>

// #include <libutil/util.h>

#define DBG_EVENT_HANDLER_CHECK 0x12345604  /*this is only used to check event handler is right*/
#define BIT_ULL(nr)             (1ULL << (nr))

//#define VRDMA_DPA_DEBUG
// #define VRDMA_RPC_TIMEOUT_ISSUE_DEBUG

#define POW2(log) (1 << (log))
#define POW2MASK(log) (POW2(log) - 1)

#define VRDMA_DPA_WQE_INLINE  (1 << 0)
#define VRDMA_DPA_WQE_WITH_IMM (1 << 1) 

#define DPA_INLINE_SEG 0x80000000
#define DPA_DMA_SEND_WQE_BB 64

#define VQP_PER_THREAD 64 
#define MAX_DPA_THREAD 8
#define VRDMA_VQP_HANDLE_BUDGET 1024 
#define VRDMA_TOTAL_WQE_BUDGET (8 * 1024)
#define VRDMA_CONT_NULL_CQE_BUDGET 6
#define VRDMA_CQ_WAIT_THRESHOLD(cq_len)  (cq_len >> 2)

enum{
	MLX5_CTRL_SEG_OPCODE_RDMA_WRITE                      = 0x8,
	MLX5_CTRL_SEG_OPCODE_RDMA_WRITE_WITH_IMMEDIATE       = 0x9,
	MLX5_CTRL_SEG_OPCODE_SEND                            = 0xa,
	MLX5_CTRL_SEG_OPCODE_RDMA_READ                       = 0x10,
};

enum{
	VRDMA_DB_CQ_LOG_DEPTH = 8,
	VRDMA_DB_CQ_ELEM_DEPTH = 6,
};

enum dpa_sync_state_t{
	VRDMA_DPA_SYNC_HOST_RDY = 1,
	VRDMA_DPA_SYNC_DEV_RDY = 2,
};

struct vrdma_dev_cqe64 {
	uint32_t emu_db_handle;
	uint32_t rsvd0[7];
	uint32_t srqn_uidx;
	uint32_t rsvd36[2];
	uint32_t byte_cnt;
	uint32_t rsvd48[2];
	uint32_t qpn;
	uint16_t wqe_counter;
	uint8_t signature;
	uint8_t op_own;
} __attribute__((packed, aligned(8)));

struct vrdma_dpa_cq {
	uint32_t cq_num;
	uint32_t log_cq_size;/*looks like we don't need*/
	flexio_uintptr_t cq_ring_daddr;
	flexio_uintptr_t cq_dbr_daddr;
	struct flexio_cq *cq;
	uint32_t overrun_ignore;
	uint32_t always_armed;
};

struct vrdma_dpa_vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vrdma_dpa_cq_ctx {
	uint32_t cqn;
	uint32_t ci;
	struct flexio_dev_cqe64 *ring;
	struct flexio_dev_cqe64 *cqe;
	uint32_t *dbr;
	uint8_t hw_owner_bit;
	uint32_t log_cq_depth;
};

/* vrdma_dpa_vq_state values:
 *
 * @VRDMA_DPA_VQ_STATE_INIT - VQ is created, but cannot handle doorbells.
 * @VRDMA_DPA_VQ_STATE_SUSPEND - VQ is suspended, no outgoing DMA, can be restarted.
 * @VRDMA_DPA_VQ_STATE_RDY - Can handle doorbells.
 * @VRDMA_DPA_VQ_STATE_ERR - VQ is in error state.
 */
enum vrdma_dpa_vq_state {
	VRDMA_DPA_VQ_STATE_INIT = 1 << 0,
	VRDMA_DPA_VQ_STATE_RDY = 1 << 1,
	VRDMA_DPA_VQ_STATE_SUSPEND = 1 << 2,
	VRDMA_DPA_VQ_STATE_ERR = 1 << 3,
};

struct vrdma_window_dev_config {
	uint32_t window_id;
	uint32_t mkey;
	flexio_uintptr_t haddr;
	flexio_uintptr_t heap_memory;
} __attribute__((__packed__, aligned(8)));


/*host rdma parameters*/
struct vrdma_host_vq_ctx {
	uint64_t rq_pi_paddr;
	uint64_t sq_pi_paddr;
	uint64_t rq_wqe_buff_pa;
	uint64_t sq_wqe_buff_pa;
	uint16_t rq_wqebb_cnt;/*maxed wqebb, pi%rq_wqebb_cnt*/
	uint16_t sq_wqebb_cnt;
	uint16_t rq_wqebb_size;
	uint16_t sq_wqebb_size;

        /*now no sf, sf_ means emu manager*/
	uint32_t sf_crossing_mkey;
	uint32_t emu_crossing_mkey;
} __attribute__((__packed__, aligned(8)));

struct vrdma_arm_vq_ctx {
	/*arm rdma parameters*/
	uint64_t rq_buff_addr;
	uint64_t sq_buff_addr;
	uint64_t rq_pi_addr;
	uint64_t sq_pi_addr;
	uint32_t rq_lkey;
	uint32_t sq_lkey;
} __attribute__((__packed__, aligned(8)));

#define VRDMA_MAX_DEBUG_COUNT 8
#define VRDMA_MAX_DEBUG_VALUE 8
struct vrdma_debug_data {
	uint32_t counter[VRDMA_MAX_DEBUG_COUNT];
	uint32_t value[VRDMA_MAX_DEBUG_VALUE];
};

struct event_handler_dma_qp_ctx {
	struct vrdma_dpa_cq qp_rqcq;
	uint16_t hw_qp_sq_pi;
	uint16_t hw_qp_sq_ci; /*get from dma_sqcq_ctx->cqe->wqe_count+1*/
	uint32_t hw_sq_size;
	uint16_t qp_num;
	uint16_t reserved1;
	flexio_uintptr_t qp_sq_buff;
	flexio_uintptr_t qp_rq_buff;
	flexio_uintptr_t dbr_daddr;
	enum vrdma_dpa_vq_state state;
};

struct vrdma_dpa_qp_info {
	uint32_t emu_db_handle;
	uint32_t vq_index;
	uint16_t valid;
	flexio_uintptr_t vqp_ctx_handle;
};

struct vrdma_dpa_event_handler_ctx {
	uint32_t dbg_signature; /*Todo: used to confirm event handler is right*/
	struct spinlock_s vqp_array_lock;

	struct vrdma_dpa_cq_ctx guest_db_cq_ctx;
	struct vrdma_dpa_cq_ctx dma_sqcq_ctx;
	struct vrdma_dpa_cq_ctx msix_cq_ctx;

	struct flexio_cq *db_handler_cq;

	uint32_t emu_outbox;
	uint32_t emu_crossing_mkey;
        /*now no sf, so sf_outbox mean emu_manager_outbox*/
	uint32_t sf_outbox;

	uint32_t window_id;
	flexio_uintptr_t window_base_addr;
	struct event_handler_dma_qp_ctx dma_qp;
	struct vrdma_debug_data debug_data;
	uint32_t ce_set_threshold;
	uint16_t vqp_count;
	struct vrdma_dpa_qp_info vqp_ctx[VQP_PER_THREAD];
};

struct vrdma_dpa_vq_data {
	struct vrdma_dpa_event_handler_ctx ehctx;
	enum dpa_sync_state_t state;
	uint8_t err;
} __attribute__((__packed__, aligned(8)));

struct vrdma_dpa_vqp_ctx {
	uint32_t emu_db_to_cq_id;
	uint32_t vq_index;
	uint16_t rq_last_fetch_start;
	uint16_t sq_last_fetch_start;
	struct vrdma_host_vq_ctx host_vq_ctx; /*host rdma parameters*/
	struct vrdma_arm_vq_ctx arm_vq_ctx; /*arm rdma parameters*/
	enum vrdma_dpa_vq_state state;
	uint16_t free_idx;
	flexio_uintptr_t eh_ctx_daddr;
};

struct vrdma_dpa_msix_send {
	uint32_t outbox_id;
	uint32_t cqn;
};

enum vrdma_dpa_vq_type {
	VRDMA_DPA_VQ_QP = 0,
	VRDMA_DPA_VQ_MAX
};

static inline void
vrdma_debug_count_set(struct vrdma_dpa_event_handler_ctx *ehctx, uint32_t cnt_idx)
{
	ehctx->debug_data.counter[cnt_idx]++;
}

static inline void
vrdma_debug_value_set(struct vrdma_dpa_event_handler_ctx *ehctx, uint32_t cnt_idx, uint32_t value)
{
	ehctx->debug_data.value[cnt_idx] = value;
}

static inline void
vrdma_debug_value_add(struct vrdma_dpa_event_handler_ctx *ehctx, uint32_t cnt_idx, uint32_t value)
{
	ehctx->debug_data.value[cnt_idx] += value;
}

#endif
