/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _VRDMA_CTRL_H
#define _VRDMA_CTRL_H
#include <stdio.h>
#include <stdbool.h>
#include "spdk/stdinc.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_srv.h"

#include "snap_vrdma_virtq.h"

#define VRDMA_EMU_NAME_PREFIX "VrdmaEmu"
#define VRDMA_EMU_NAME_MAXLEN 32
#define VRDMA_DMA_ELEM_SIZE 64

extern struct vrdma_state_machine vrdma_sm;

struct snap_vrdma_ctrl;
struct snap_context;

struct vrdma_ctrl {
    char name[VRDMA_EMU_NAME_MAXLEN];
    char emu_manager[SPDK_EMU_MANAGER_NAME_MAXLEN];
    size_t nthreads;
    int pf_id;
    uint32_t dev_inited:1;
    struct vrdma_dev dev;
    struct spdk_vrdma_dev *vdev;
    struct snap_context *sctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct vrdma_admin_sw_qp sw_qp;
    struct snap_vrdma_ctrl *sctrl;
    /** Service-specific callbacks. */
	const struct vRdmaServiceOps *srv_ops;
    void (*destroy_done_cb)(void *arg);
    void *destroy_done_cb_arg;
};

struct vrdma_ctrl_init_attr {
    const char *emu_manager_name;
    int pf_id;
    struct spdk_vrdma_dev *vdev;
    uint32_t nthreads;
    bool force_in_order;
    bool suspended;
};

enum vrdma_sq_sm_state_type {
        VRDMA_SQ_STATE_IDLE,
        VRDMA_SQ_STATE_INIT_CI,
        VRDMA_SQ_STATE_POLL_PI,
        VRDMA_SQ_STATE_HANDLE_PI,
        VRDMA_SQ_STATE_WQE_READ,
        VRDMA_SQ_STATE_WQE_PARSE,
        VRDMA_SQ_STATE_WQE_MAP_BACKEND,
        VRDMA_SQ_STATE_WQE_SUBMIT,
        VRDMA_SQ_STATE_FATAL_ERR,
        VRDMA_SQ_NUM_OF_STATES,
};

enum vrdma_rq_sm_state_type {
    VRDMA_RQ_STATE_IDLE,
    VRDMA_RQ_STATE_INIT_CI,
    VRDMA_RQ_STATE_FATAL_ERR,
    VRDMA_RQ_NUM_OF_STATES,
};

struct vrdma_q_comm {
	uint64_t wqe_buff_pa;
	uint64_t doorbell_pa;
	uint16_t wqebb_size:2; /* based on 64 * (sq_wqebb_size + 1) */
	uint16_t pagesize:5; /* 12, 21, 30 | 2 ^ (n) */
	uint16_t hop:2;
	uint16_t reserved:7;
	uint16_t wqebb_cnt; /* sqe entry cnt */
	uint32_t qpn;
	uint32_t cqn;
	uint32_t mqpn[4];
	uint16_t pi;
	uint16_t pre_pi;
	uint32_t num_to_parse;
};

struct vrdma_sq {
	struct vrdma_q_comm comm;
	struct snap_dma_completion q_comp;
	struct snap_vrdma_queue *snap_queue;
	struct vrdma_sq_state_machine *custom_sm;
	enum vrdma_sq_sm_state_type sm_state;
	struct ibv_mr *mr;
	struct vrdma_send_wqe *sq_buff; /* wqe buff */
};

struct vrdma_rq {
	struct vrdma_q_comm comm;
	struct snap_dma_completion q_comp;
	struct snap_vrdma_queue *snap_queue;
	struct vrdma_rq_state_machine *custom_sm;
	enum vrdma_rq_sm_state_type sm_state;
	struct ibv_mr *mr;
	struct vrdma_recv_wqe *rq_buff; /* wqe buff */
};

int vrdma_ctrl_adminq_progress(void *ctrl);
void vrdma_ctrl_progress(void *ctrl);
int vrdma_ctrl_progress_all_io(void *ctrl);
int vrdma_ctrl_progress_io(void *arg, int thread_id);
void vrdma_ctrl_suspend(void *ctrl);
bool vrdma_ctrl_is_suspended(void *ctrl);

struct vrdma_ctrl *
vrdma_ctrl_init(const struct vrdma_ctrl_init_attr *attr);
void vrdma_ctrl_destroy(void *arg, void (*done_cb)(void *arg),
                             void *done_cb_arg);
int vrdma_dev_name_to_id(const char *rdma_dev_name);
#endif
