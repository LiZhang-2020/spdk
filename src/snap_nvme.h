/*
 * Copyright (c) 2012 Mellanox Technologies, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SNAP_NVME_H
#define SNAP_NVME_H

#include <stdlib.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <pthread.h>
#include <linux/types.h>

#include "snap.h"

struct snap_nvme_device;
struct snap_nvme_cq;

struct snap_nvme_namespace_attr {
	int			src_nsid;
	int			dst_nsid;
	int			lba_size;
	int			md_size;
};

struct snap_nvme_namespace {
	struct mlx5_snap_devx_obj		*ns;
	int					src_id;
	int					dst_id;
	TAILQ_ENTRY(snap_nvme_namespace)	entry;
};

enum snap_nvme_queue_type {
	SNAP_NVME_RAW_MODE	= 1 << 0,
	SNAP_NVME_TO_NVMF_MODE	= 1 << 1,
};

enum snap_nvme_sq_modify {
	SNAP_NVME_SQ_MOD_QPN	= 1 << 0,
	SNAP_NVME_SQ_MOD_STATE	= 1 << 1,
};

enum snap_nvme_sq_state {
	SNAP_NVME_SQ_STATE_INIT	= 1 << 0,
	SNAP_NVME_SQ_STATE_RDY	= 1 << 1,
	SNAP_NVME_SQ_STATE_ERR	= 1 << 2,
};

struct snap_nvme_sq_attr {
	enum snap_nvme_queue_type	type;
	uint32_t			id;
	uint16_t			queue_depth;
	uint64_t			base_addr;
	uint64_t			modifiable_fields;//mask of snap_nvme_sq_modify
	struct ibv_qp			*qp;
	uint32_t			emulated_device_dma_mkey;
	struct snap_nvme_cq		*cq;
	enum snap_nvme_sq_state		state;
	uint8_t				log_entry_size;
};

struct snap_nvme_sq {
	struct ibv_context			*rdma_dev;
	uint32_t				id;
	struct mlx5_snap_devx_obj		*sq;
	struct mlx5_snap_hw_qp			*hw_qp;
	uint64_t				mod_allowed_mask;
};

struct snap_nvme_cq_attr {
	enum snap_nvme_queue_type	type;
	uint32_t			id;
	uint16_t			msix;
	uint16_t			queue_depth;
	uint64_t			base_addr;
	uint16_t			cq_period;
	uint16_t			cq_max_count;
	uint8_t				log_entry_size;
};

struct snap_nvme_cq {
	uint32_t				id;
	struct mlx5_snap_devx_obj		*cq;
};

enum snap_nvme_device_modify {
	SNAP_NVME_DEV_MOD_BAR	= 1 << 0,
};

struct snap_nvme_device_attr {
	bool					enabled;
	struct snap_nvme_registers		bar;
	uint64_t				modifiable_fields;//mask of snap_nvme_device_modify
	int					num_of_vfs;
};

struct snap_nvme_device {
	struct snap_device			*sdev;
	uint32_t				num_queues;
	uint32_t				db_base;

	pthread_mutex_t				lock;
	TAILQ_HEAD(, snap_nvme_namespace)	ns_list;

	struct snap_nvme_cq			*cqs;
	struct snap_nvme_sq			*sqs;
};

int snap_nvme_init_device(struct snap_device *sdev);
int snap_nvme_teardown_device(struct snap_device *sdev);
int snap_nvme_query_device(struct snap_device *sdev,
	struct snap_nvme_device_attr *attr);
int snap_nvme_modify_device(struct snap_device *sdev, uint64_t mask,
		struct snap_nvme_device_attr *attr);

struct snap_nvme_namespace*
snap_nvme_create_namespace(struct snap_device *sdev,
		struct snap_nvme_namespace_attr *attr);
int snap_nvme_destroy_namespace(struct snap_nvme_namespace *ns);
struct snap_nvme_cq*
snap_nvme_create_cq(struct snap_device *sdev, struct snap_nvme_cq_attr *attr);
int snap_nvme_destroy_cq(struct snap_nvme_cq *cq);
struct snap_nvme_sq*
snap_nvme_create_sq(struct snap_device *sdev, struct snap_nvme_sq_attr *attr);
int snap_nvme_destroy_sq(struct snap_nvme_sq *sq);
int snap_nvme_query_sq(struct snap_nvme_sq *sq,
	struct snap_nvme_sq_attr *attr);
int snap_nvme_modify_sq(struct snap_nvme_sq *sq, uint64_t mask,
	struct snap_nvme_sq_attr *attr);

#endif
