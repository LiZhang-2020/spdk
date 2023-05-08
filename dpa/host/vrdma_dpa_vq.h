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

#ifndef __VRDMA_DPA_VQ_H__
#define __VRDMA_DPA_VQ_H__

#include <infiniband/mlx5dv.h>
#include <libflexio/flexio.h>
#include "dpa/vrdma_dpa_common.h"

struct vrdma_dpa_ctx;
struct vrdma_prov_vq_init_attr;
enum vrdma_dpa_vq_state;
struct vrdma_dpa_thread_ctx;

extern struct vrdma_dpa_thread_ctx *g_dpa_threads;

struct virtnet_vq_common_config {
	uint16_t size;
	uint16_t msix_vector;
	uint16_t enable;
	uint16_t notify_off;
	uint64_t desc;
	uint64_t driver;
	uint64_t device;
};

struct vrdma_prov_thread_init_attr {
	uint32_t dma_tx_qsize;
	uint16_t dma_tx_elem_size;
	uint32_t dma_rx_qsize;
	uint16_t dma_rx_elem_size;

	struct ibv_context *emu_ib_ctx;
	struct ibv_pd *emu_pd;
	uint32_t emu_mkey;
	uint16_t emu_vhca_id;

	struct ibv_context *sf_ib_ctx;
	struct ibv_pd *sf_pd;
	uint32_t sf_mkey;
	uint16_t sf_vhca_id;
	
	uint16_t num_msix;
};

struct vrdma_dpa_dma_qp_attr {
	uint16_t vq_idx;
	uint32_t tisn_or_qpn;
	uint32_t tx_qsize;
	uint32_t tx_elem_size;
	uint32_t rx_qsize;
	uint32_t rx_elem_size;

	struct ibv_context *sf_ib_ctx;
	struct ibv_pd *sf_pd;
};

struct vrdma_prov_vqp_init_attr {
	uint16_t vq_idx;
	uint32_t qdb_idx;
	uint16_t sf_vhca_id;
	uint16_t emu_vhca_id;
	uint32_t emu_db_handler;
	
	struct vrdma_host_vq_ctx host_vq_ctx; /*host rdma parameters*/
	struct vrdma_arm_vq_ctx arm_vq_ctx; /*arm rdma parameters*/
};

struct vrdma_dpa_dma_qp {
	struct flexio_qp *qp;
	struct flexio_mkey *rqd_mkey;
	struct flexio_mkey *sqd_mkey;
	flexio_uintptr_t buff_daddr;
	flexio_uintptr_t rq_daddr;
	flexio_uintptr_t sq_daddr;
	flexio_uintptr_t dbr_daddr;
	flexio_uintptr_t rx_wqe_buff;
	flexio_uintptr_t tx_wqe_buff;
	enum dpa_sync_state_t state; /*used for dma_qp state*/
	uint32_t log_sq_depth;
	uint32_t log_rq_depth;
	int qp_num;
	struct vrdma_dpa_cq dma_q_rqcq;
	struct vrdma_dpa_cq dma_q_sqcq;
};

struct vrdma_dpa_handler {
	struct flexio_event_handler *db_handler; /* used to receive db and get pi/wqe*/
	flexio_func_t *db_handler_func;
	struct flexio_event_handler *rq_dma_q_handler; /*used to send msix*/
	flexio_func_t *rq_dma_q_handler_func;
	struct vrdma_dpa_cq db_cq;
	struct flexio_msix *msix;
	uint16_t sq_msix_vector;
	uint16_t rq_msix_vector;
};

struct vrdma_dpa_thread_ctx {
	struct vrdma_dpa_ctx *dpa_ctx;
	struct vrdma_dpa_handler *dpa_handler;
	flexio_uintptr_t heap_memory;
	struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx;
    /*now don't have sf, so sf_mkey means emu manager mkey*/
	uint32_t sf_mkey;
	uint32_t emu_mkey;
	struct snap_vrdma_queue *sw_dma_qp;
	struct vrdma_dpa_dma_qp *dpa_dma_qp;
	struct vrdma_dpa_event_handler_ctx *eh_ctx;
	uint16_t attached_vqp_num;
	uint16_t thread_idx;
};

struct vrdma_dpa_vqp {
	flexio_uintptr_t dpa_heap_memory; /* qp ctx addr in device side */
	uint32_t emu_db_to_cq_id;
	uint32_t ctx_idx;
	struct mlx5dv_devx_obj *devx_emu_db_to_cq_ctx;
	struct vrdma_dpa_thread_ctx *dpa_thread;
};

struct vrdma_msix_init_attr {
	struct ibv_context *emu_ib_ctx;
	uint16_t emu_vhca_id;
        /*now no sf, so sf_ means emu_manager_*/
	struct ibv_context *sf_ib_ctx;
	uint16_t sf_vhca_id;
	uint16_t msix_vector;
	uint32_t cq_size;
}; 

// int virtnet_dpa_vq_init(struct virtnet_dpa_vq *dpa_vq,
// 			struct virtnet_dpa_ctx *dpa_ctx,
// 			struct ibv_context *emu_ibv_ctx,
// 			const char *vq_handler,
// 			flexio_uintptr_t *dpa_daddr);
// void virtnet_dpa_vq_uninit(struct virtnet_dpa_vq *dpa_vq);

// int virtnet_dpa_db_cq_create(struct flexio_process *process,
// 			     struct ibv_context *emu_ibv_ctx,
// 			     struct flexio_event_handler *event_handler,
// 			     struct virtnet_dpa_cq *dpa_cq,
// 			     uint32_t emu_uar_id);
// void virtnet_dpa_db_cq_destroy(struct virtnet_dpa_vq *dpa_vq);

// int virtnet_dpa_vq_state_modify(struct virtnet_dpa_vq *dpa_vq,
// 				enum virtnet_dpa_vq_state vq_state);
// int
// virtnet_dpa_vq_event_handler_init(const struct virtnet_dpa_vq *dpa_vq,
// 				  struct virtnet_dpa_ctx *dpa_ctx,
// 				  struct virtnet_prov_vq_init_attr *attr,
// 				  struct virtnet_dpa_emu_dev_ctx *emu_dev_ctx);
int vrdma_dpa_vq_pup_func_register(struct vrdma_dpa_ctx *dpa_ctx);
void vrdma_dpa_vq_pup_func_deregister(struct vrdma_dpa_ctx *dpa_ctx);
int vrdma_dpa_msix_create(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    			struct vrdma_msix_init_attr *attr,
			    			int max_msix);

void vrdma_dpa_msix_destroy(uint16_t msix_vector,
			      struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx);
int vrdma_dpa_set_vq_repost_pi(void *vqp, uint16_t vq_repost_pi);

#endif
