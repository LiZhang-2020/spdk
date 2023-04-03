/*
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#ifndef __VRDMA_DPA_MM_H__
#define __VRDMA_DPA_MM_H__

#include <libflexio/flexio.h>
#include <infiniband/mlx5dv.h>
#include "dpa/vrdma_dpa_common.h"

enum vrdma_dpa_ring_size {
	VRDMA_DPA_CQE_BSIZE = 6,
	VRDMA_DPA_SWQE_BSIZE = VRDMA_DPA_CQE_BSIZE,
};

enum vrdma_dpa_buff_size {
	VRDMA_DPA_QP_RQ_BUFF_SIZE = 64,
};

int vrdma_dpa_mm_zalloc(struct flexio_process *process, size_t buff_bsize,
			  flexio_uintptr_t *dest_daddr_p);
int vrdma_dpa_mm_free(struct flexio_process *process, flexio_uintptr_t daddr);
flexio_uintptr_t vrdma_dpa_mm_dbr_alloc(struct flexio_process *process);
int vrdma_dpa_mm_cq_alloc(struct flexio_process *process, int cq_size,
			  struct vrdma_dpa_cq *cq);
void vrdma_dpa_mm_cq_free(struct flexio_process *process,
			    struct vrdma_dpa_cq *cq);
flexio_uintptr_t vrdma_dpa_mm_qp_buff_alloc(struct flexio_process *process,
					      int log_rq_depth,
					      flexio_uintptr_t *rq_daddr,
					      int log_sq_depth,
					      flexio_uintptr_t *sq_daddr);
void vrdma_dpa_mm_qp_buff_free(struct flexio_process *process,
				 flexio_uintptr_t buff_daddr);
int vrdma_dpa_init_qp_rx_ring(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
									struct vrdma_dpa_dma_qp *dpa_dma_qp,
									uint32_t num_of_wqes,
									uint32_t wqe_stride,
									uint32_t elem_size,
									uint32_t mkey_id);
int vrdma_dpa_mkey_create(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    struct flexio_qp_attr *qp_attr,
			    uint32_t data_bsize,
				flexio_uintptr_t wqe_buff,
			    struct flexio_mkey **mkey);
void vrdma_dpa_mkey_destroy(struct flexio_mkey *mkey);
#endif
