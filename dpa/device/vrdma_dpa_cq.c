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

#include <libflexio-libc/string.h>
#include <libflexio-libc/stdio.h>
#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include "vrdma_dpa_dev_com.h"
#include "vrdma_dpa_cq.h"
#include "../vrdma_dpa_common.h"

static int vrdma_dpa_is_hw_owner(struct vrdma_dpa_cq_ctx *cq_ctx,
				   struct flexio_dev_cqe64 *cqe)
{
	return ((flexio_dev_cqe_get_owner(cqe) ^ cq_ctx->hw_owner_bit) & 0x1);
}

void vrdma_dpa_cq_incr(struct vrdma_dpa_cq_ctx *cq_ctx, uint16_t mask)
{
	cq_ctx->ci++;
	cq_ctx->cqe = &cq_ctx->ring[cq_ctx->ci & mask];
	/* check for wrap around */
	if (!(cq_ctx->ci & mask))
		cq_ctx->hw_owner_bit = !cq_ctx->hw_owner_bit;
}

volatile struct flexio_dev_cqe64 *
vrdma_dpa_cqe_get(struct vrdma_dpa_cq_ctx *cq_ctx, uint16_t mask)
{
	struct flexio_dev_cqe64 *cqe;

	cqe = &(cq_ctx->ring)[cq_ctx->ci & mask];

	if (vrdma_dpa_is_hw_owner(cq_ctx, cqe))
		return NULL;

	cq_ctx->ci++;
	if ((cq_ctx->ci & mask) == 0)
		cq_ctx->hw_owner_bit ^= 0x1;

	return cqe;
}

void vrdma_dpa_cq_wait(struct vrdma_dpa_cq_ctx *cq_ctx, uint16_t mask,
			uint16_t *comp_wqe_idx)
{
	struct flexio_dev_cqe64 *cqe;
	uint8_t opcode;
	uint32_t ci;

	ci = cq_ctx->ci & mask;
	cqe = &cq_ctx->ring[ci];

	do {
		/* Ensure that previously read CQE is invalidated to force
		 * read again that might have updated value.
		 */
		fence_r();
	} while (vrdma_dpa_is_hw_owner(cq_ctx, cqe));

	opcode = vrdma_dpa_cqe_get_opcode(cqe);
	if (opcode == MLX5_CQE_RESP_ERR ||
	    opcode == MLX5_CQE_REQ_ERR) {
		printf("---vrdma_dpa_cq_wait cqe err %d\n", opcode);
	}

	*comp_wqe_idx = be16_to_cpu(cqe->wqe_counter);
	cq_ctx->ci++;

	/* owner bit wraparound */
	if ((cq_ctx->ci & mask) == 0)
		cq_ctx->hw_owner_bit = cq_ctx->hw_owner_bit ^ 0x01;
}
