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

#include <infiniband/verbs.h>

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "snap.h"
#include "snap_vrdma_ctrl.h"
#include "snap_vrdma_virtq.h"

#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_qp.h"
#include "vrdma_providers.h"

// #define NO_PERF_DEBUG
/* TODO: use a hash table or sorted list */
struct vrdma_tgid_list_head vrdma_tgid_list =
                LIST_HEAD_INITIALIZER(vrdma_tgid_list);

struct vrdma_tgid_node *
vrdma_find_tgid_node(union ibv_gid *remote_tgid, union ibv_gid *local_tgid)
{
    struct vrdma_tgid_node *tmp_node, *tgid_node = NULL;

    LIST_FOREACH_SAFE(tgid_node, &vrdma_tgid_list, entry, tmp_node)
        if ((memcmp(&tgid_node->key.remote_tgid, remote_tgid, sizeof(*remote_tgid)) == 0) &&
            (memcmp(&tgid_node->key.local_tgid, local_tgid, sizeof(*local_tgid)) == 0))
            break;
    return tgid_node;
}

static void
vrdma_dump_mqp(struct vrdma_tgid_node *tgid_node, int8_t mqp_idx, int32_t meta_start)
{
    if (!tgid_node) return;
    struct vrdma_backend_qp *mqp = NULL;
    struct vrdma_vqp *vqp_entry = NULL;
    struct mqp_sq_meta *sq_meta = NULL;
    int32_t i;

    mqp = tgid_node->src_udp[mqp_idx].mqp;
    if (mqp) {
        printf("--------------mqp[%u]=%p--------------\n", mqp_idx, mqp);
        printf("\tpoller_core=%u udp_sport=0x%x qpn=0x%x qp_state=%u\n",
               mqp->poller_core, tgid_node->src_udp[mqp_idx].udp_src_port,
               mqp->bk_qp.qpnum, mqp->qp_state);
        printf("\tvqp_cnt=%u vqp list:\n", mqp->vqp_cnt);
        pthread_spin_lock(&mqp->vqp_list_lock);
        LIST_FOREACH(vqp_entry, &mqp->vqp_list, entry) {
            printf("\t\t%u\n", vqp_entry->qpn);
        }
        pthread_spin_unlock(&mqp->vqp_list_lock);
        printf("\tmqp.sq_pi=%u mqp.sq_ci=%u mqp.avg_depth=%u\n",
               mqp->bk_qp.hw_qp.sq.pi, mqp->bk_qp.sq_ci, mqp->avg_depth);
        for (i = 0; i < MQP_DEPTH_SAMPLE_NUM; i++) {
            printf("\tmqp->sample_depth[%u]=%u\n", i, mqp->sample_depth[i]);
        }
        if (is_vrdma_vqp_migration_enable()) {
            printf("\tpmtu=%u, msg_1st_psn=%u, rnxt_rcv_psn=%u, mig_lnxt_rcv_psn=%u\n",
                   mqp->mig_ctx.mig_pmtu, mqp->mig_ctx.msg_1st_psn,
                   mqp->mig_ctx.mig_rnxt_rcv_psn, mqp->mig_ctx.mig_lnxt_rcv_psn);
            if (meta_start == -1)
                return;
            for (i = meta_start; i < mqp->bk_qp.hw_qp.sq.pi; i++) {
                sq_meta = &mqp->sq_meta_buf[i%mqp->bk_qp.hw_qp.sq.wqe_cnt];
                printf("\tsq_meta=%p, sq_meta[%u]: vqp=%u, twqe_idx=%u, first_psn=%u, last_psn=%u\n",
                       sq_meta, i, sq_meta->vqp->qp_idx, sq_meta->twqe_idx, sq_meta->first_psn, sq_meta->last_psn);
            }
        }
    }
}

void
vrdma_dump_tgid_node(struct vrdma_tgid_node *tgid_node,
                     int32_t specified_mqp,
                     int32_t meta_start)
{
    uint32_t mqp_idx;

    if (!tgid_node) return;
    printf("\n========= vrdma tgid node=%p debug =========\n", tgid_node);
    printf("key:\tremote_tgid.prefix=0x%llx \tremote_tgid.ip=0x%llx\n"
           "\tlocal_tgid.prefix=0x%llx \tlocal_tgid.ip=0x%llx\n"
           "\tmax_mqp_cnt=%u, curr_mqp_cnt=%u\n",
           tgid_node->key.remote_tgid.global.subnet_prefix,
           tgid_node->key.remote_tgid.global.interface_id,
           tgid_node->key.local_tgid.global.subnet_prefix,
           tgid_node->key.local_tgid.global.interface_id,
           tgid_node->max_mqp_cnt, tgid_node->curr_mqp_cnt);
    if (specified_mqp == -1) {
        for (mqp_idx = 0; mqp_idx < tgid_node->curr_mqp_cnt; mqp_idx++) {
            vrdma_dump_mqp(tgid_node, mqp_idx, meta_start);
        }
    } else {
        vrdma_dump_mqp(tgid_node, specified_mqp, meta_start);
    }
    printf("========= dump tgid node end =========\n");
}

void
vrdma_destroy_tgid_list(void)
{
    struct vrdma_tgid_node *tmp_node, *tgid_node = NULL;
    uint8_t i;

    LIST_FOREACH_SAFE(tgid_node, &vrdma_tgid_list, entry, tmp_node) {
        LIST_REMOVE(tgid_node, entry);
        for (i = 0; i < tgid_node->curr_mqp_cnt; i++) {
            vrdma_destroy_backend_qp(&tgid_node->src_udp[i].mqp);
        }
        free(tgid_node->src_udp);
        free(tgid_node);
    }
}

struct vrdma_tgid_node *
vrdma_create_tgid_node(union ibv_gid *remote_tgid,
                       union ibv_gid *local_tgid,
                       struct vrdma_ctrl *ctrl,
                       uint16_t udp_sport_start,
                       uint32_t max_mqp_cnt)
{
    struct vrdma_tgid_node *tgid_node = NULL;
    uint32_t i;

    tgid_node = calloc(1, sizeof(struct vrdma_tgid_node));
    if (!tgid_node) {
        SPDK_ERRLOG("Failed to create tgid_node\n");
        return NULL;
    }
    SPDK_NOTICELOG("created new tgid_node, max_mqp_cnt=%u\n", max_mqp_cnt);
    memcpy(&tgid_node->key.local_tgid, local_tgid, sizeof(union ibv_gid));
    memcpy(&tgid_node->key.remote_tgid, remote_tgid, sizeof(union ibv_gid));
    tgid_node->ctrl = ctrl;
    tgid_node->max_mqp_cnt = max_mqp_cnt;
    tgid_node->src_udp = calloc(max_mqp_cnt, sizeof(struct vrdma_udp_sport_node));
    if (!tgid_node->src_udp) {
        free(tgid_node);
        SPDK_ERRLOG("Failed to create vrdma_udp_sport_node\n");
        return NULL;
    }
    for(i = 0; i < max_mqp_cnt; i++) {
        tgid_node->src_udp[i].udp_src_port = udp_sport_start + i;
    }
    tgid_node->pd = ctrl->vdev->vrdma_sf.sf_pd;
    LIST_INSERT_HEAD(&vrdma_tgid_list, tgid_node, entry);
    return tgid_node;
}

struct spdk_vrdma_qp *
find_spdk_vrdma_qp_by_idx(struct vrdma_ctrl *ctrl, uint32_t qp_idx)
{
	struct spdk_vrdma_qp *vqp_tmp, *vqp = NULL;

	LIST_FOREACH_SAFE(vqp, &ctrl->vdev->vqp_list, entry, vqp_tmp) {
        if (vqp->qp_idx == qp_idx)
            break;
	}
	return vqp;
}

struct vrdma_backend_qp *
vrdma_find_mqp_by_depth(struct vrdma_tgid_node *tgid_node, uint8_t *mqp_idx)
{
    struct vrdma_backend_qp *mqp = NULL, *tmp_mqp = NULL;
    uint8_t i;

    for (i = 0; i < tgid_node->max_mqp_cnt; i++) {
        tmp_mqp = tgid_node->src_udp[i].mqp;
        if (tmp_mqp && tmp_mqp->qp_state != IBV_QPS_ERR &&
                (!mqp || tmp_mqp->avg_depth < mqp->avg_depth)) {
            mqp = tgid_node->src_udp[i].mqp;
            *mqp_idx = i;
        }
    }
    return mqp;
}

struct vrdma_backend_qp *
vrdma_find_mqp(struct vrdma_ctrl *ctrl,
               struct vrdma_tgid_node *tgid_node,
               uint8_t *mqp_idx)
{
    struct vrdma_backend_qp *mqp = NULL, *tmp_mqp = NULL;
    uint8_t i;

    if (!tgid_node || !mqp_idx) return NULL;
    if (tgid_node->curr_mqp_cnt < tgid_node->max_mqp_cnt) {
        *mqp_idx = tgid_node->curr_mqp_cnt;
        mqp = vrdma_create_backend_qp(tgid_node, *mqp_idx);
        if (!mqp) {
            SPDK_ERRLOG("failed to create bankend qp\n");
            return NULL;
        }
        if (vrdma_modify_backend_qp_to_init(mqp)) {
            vrdma_destroy_backend_qp(&mqp);
            return NULL;
        }
        if (vrdma_qp_notify_remote_by_rpc(ctrl, tgid_node, *mqp_idx)) {
            SPDK_ERRLOG("failed to send rpc\n");
            vrdma_destroy_backend_qp(&mqp);
            tgid_node->src_udp[*mqp_idx].mqp = NULL;
            return NULL;
        }
        tgid_node->curr_mqp_cnt++;
        return mqp;
    } else {
        for (i = 0; i < tgid_node->max_mqp_cnt; i++) {
            tmp_mqp = tgid_node->src_udp[i].mqp;
            if (!mqp || (tmp_mqp && tmp_mqp->qp_state != IBV_QPS_ERR &&
                         tmp_mqp->vqp_cnt < mqp->vqp_cnt)) {
                mqp = tgid_node->src_udp[i].mqp;
                *mqp_idx = i;
            }
        }
    }
    return mqp;
}

int vrdma_mqp_add_vqp_to_list(struct vrdma_backend_qp *mqp,
                              struct spdk_vrdma_qp *vqp,
                              uint32_t vqp_idx)
{
    struct vrdma_vqp *vqp_entry = NULL;
    vqp_entry = calloc(1, sizeof(struct vrdma_vqp));
    if (!vqp_entry) {
        SPDK_ERRLOG("Failed to allocate qpn memory");
        return -1;
    }
    vqp_entry->qpn = vqp_idx;
    vqp_entry->vqp = vqp;
    vqp->pre_bk_qp = mqp;
    if (mqp->qp_state == IBV_QPS_RTS)
        vqp->bk_qp = mqp;
    pthread_spin_lock(&mqp->vqp_list_lock);
    LIST_INSERT_HEAD(&mqp->vqp_list, vqp_entry, entry);
    pthread_spin_unlock(&mqp->vqp_list_lock);
    mqp->vqp_cnt++;
    SPDK_NOTICELOG("vqp=%u, mqp=0x%x, mqp->vqp_cnt=%u\n",
                   vqp_idx, mqp->bk_qp.qpnum, mqp->vqp_cnt);
    return 0;
}

void
vrdma_mqp_del_vqp_from_list(struct vrdma_backend_qp *mqp,
                            uint32_t vqp_idx)
{
    struct vrdma_vqp *vqp_entry = NULL, *tmp;

	if(!mqp) return;
    pthread_spin_lock(&mqp->vqp_list_lock);
    LIST_FOREACH_SAFE(vqp_entry, &mqp->vqp_list, entry, tmp) {
        if (vqp_entry->qpn == vqp_idx) {
            vqp_entry->vqp->bk_qp = NULL;
            vqp_entry->vqp->pre_bk_qp = NULL;
            LIST_REMOVE(vqp_entry, entry);
        }
    }
    pthread_spin_unlock(&mqp->vqp_list_lock);
    free(vqp_entry);
    mqp->vqp_cnt--;
    //SPDK_NOTICELOG("vqp=0x%x, mqp=0x%x, mqp->vqp_cnt=%u\n",
     //              vqp_idx, mqp->bk_qp.qpnum, mqp->vqp_cnt);
    return;
}

struct spdk_vrdma_qp *
vrdma_mqp_find_vqp(struct vrdma_backend_qp *mqp,
                   uint32_t vqp_idx)
{
    struct vrdma_vqp *vqp_entry = NULL;
    pthread_spin_lock(&mqp->vqp_list_lock);
    LIST_FOREACH(vqp_entry, &mqp->vqp_list, entry) {
        if (vqp_entry->qpn == vqp_idx) {
            pthread_spin_unlock(&mqp->vqp_list_lock);
            return vqp_entry->vqp;
        }
    }
    pthread_spin_unlock(&mqp->vqp_list_lock);
    return NULL;
}

void
set_spdk_vrdma_bk_qp_active(struct vrdma_backend_qp *bk_qp)
{
    struct vrdma_vqp *vqp_entry = NULL;
    pthread_spin_lock(&bk_qp->vqp_list_lock);
    LIST_FOREACH(vqp_entry, &bk_qp->vqp_list, entry) {
        vqp_entry->vqp->bk_qp = bk_qp;
#ifdef MPATH_DBG
        SPDK_NOTICELOG("%s vqp=0x%x mqp=0x%x: \n", __func__,
                       vqp_entry->qpn, bk_qp->bk_qp.qpnum);
#endif
    }
    pthread_spin_unlock(&bk_qp->vqp_list_lock);
}

struct vrdma_backend_qp *
vrdma_create_backend_qp(struct vrdma_tgid_node *tgid_node,
                        uint8_t mqp_idx)
{
	struct vrdma_backend_qp *qp;

    if (tgid_node->src_udp[mqp_idx].mqp) {
        SPDK_ERRLOG("Already had backend QP on mqp_idx=0x%x for tgid_node", mqp_idx);
        return NULL;
    }
	qp = calloc(1, sizeof(*qp));
    if (!qp) {
		SPDK_ERRLOG("Failed to allocate backend QP memory");
		return NULL;
	}
    LIST_INIT(&qp->vqp_list);
    pthread_spin_init(&qp->vqp_list_lock, PTHREAD_PROCESS_PRIVATE);
    qp->tgid_node = tgid_node;
	qp->pd = tgid_node->pd;
	qp->poller_core = mqp_idx;
	qp->remote_qpn = VRDMA_INVALID_QPN;
	qp->bk_qp.qp_attr.qp_type = SNAP_OBJ_DEVX;
	qp->bk_qp.qp_attr.sq_size = VRDMA_BACKEND_QP_SQ_SIZE;
	qp->bk_qp.qp_attr.sq_max_sge = 1;
	qp->bk_qp.qp_attr.sq_max_inline_size = 256;
	qp->bk_qp.qp_attr.rq_size = VRDMA_BACKEND_QP_RQ_SIZE;
	qp->bk_qp.qp_attr.rq_max_sge = 1;
	qp->bk_qp.qp_attr.is_vrdma = 1;
	if (snap_vrdma_create_qp_helper(qp->pd, &qp->bk_qp)) {
		SPDK_ERRLOG("Failed to create backend QP ");
		goto free_bk_qp;
	}
	qp->sq_meta_buf = calloc(qp->bk_qp.qp_attr.sq_size,
	                         sizeof(struct mqp_sq_meta));
	if (!qp->sq_meta_buf) {
        SPDK_ERRLOG("Failed to allocate sq_meta_buf\n");
        goto free_bk_qp;
	}
    tgid_node->src_udp[mqp_idx].mqp = qp;
    qp->qp_state = IBV_QPS_INIT;
	SPDK_NOTICELOG("create mqp=0x%x mqp_idx=%u sucessfully\n", qp->bk_qp.qpnum, mqp_idx);
	return qp;

free_bk_qp:
	free(qp);
	return NULL;
}

int vrdma_query_bankend_qp_next_rcv_psn(struct vrdma_backend_qp *bk_qp,
                                        uint32_t *next_rcv_psn)
{
    struct snap_qp *sqp;

    sqp = bk_qp->bk_qp.sqp;
    if (snap_vrdma_query_bankend_qp_next_rcv_psn(sqp, next_rcv_psn)) {
        SPDK_ERRLOG("Failed to query bankend QP=0x%x",
                    bk_qp->bk_qp.qpnum);
        return -1;
    }
    SPDK_NOTICELOG("Succeeded to query bankend QP=0x%x next_rcv_psn=%u",
                   bk_qp->bk_qp.qpnum, *next_rcv_psn);
    return 0;
}

int vrdma_modify_backend_qp_to_err(struct vrdma_backend_qp *bk_qp)
{
    struct snap_qp *sqp;

    sqp = bk_qp->bk_qp.sqp;
    if (snap_vrdma_modify_bankend_qp_2err(sqp)) {
        SPDK_ERRLOG("Failed to modify bankend QP=0x%x to error",
                    bk_qp->bk_qp.qpnum);
        return -1;
    }
    bk_qp->qp_state = IBV_QPS_ERR;
    SPDK_NOTICELOG("Succeeded to modify bankend QP=0x%x to error",
                   bk_qp->bk_qp.qpnum);
    return 0;
}

int vrdma_modify_backend_qp_to_init(struct vrdma_backend_qp *bk_qp)
{
	struct ibv_qp_attr qp_attr = {0};
	struct snap_qp *sqp;
	int attr_mask;

	sqp = bk_qp->bk_qp.sqp;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_LOCAL_WRITE;
	attr_mask = IBV_QP_ACCESS_FLAGS;
	if (snap_vrdma_modify_bankend_qp_rst2init(sqp, &qp_attr, attr_mask)) {
		SPDK_ERRLOG("Failed to modify bankend QP=0x%x reset to init",
		            bk_qp->bk_qp.qpnum);
		return -1;
	}
	bk_qp->qp_state = IBV_QPS_INIT;
    SPDK_NOTICELOG("Succeeded to modify bankend QP=0x%x reset to init",
                   bk_qp->bk_qp.qpnum);
	return 0;
}

int vrdma_modify_backend_qp_to_rtr(struct vrdma_backend_qp *bk_qp,
				struct ibv_qp_attr *qp_attr, int attr_mask,
			    struct snap_vrdma_bk_qp_rdy_attr *rdy_attr)
{
	struct snap_qp *sqp;
	sqp = bk_qp->bk_qp.sqp;
	bk_qp->remote_qpn = qp_attr->dest_qp_num;

	if (snap_vrdma_modify_bankend_qp_init2rtr(sqp, qp_attr, attr_mask, rdy_attr)) {
		SPDK_ERRLOG("Failed to modify bankend QP=0x%x init to RTR",
		            bk_qp->bk_qp.qpnum);
		return -1;
	}
	bk_qp->qp_state = IBV_QPS_RTR;
    SPDK_NOTICELOG("Succeeded to modify bankend QP=0x%x init to RTR, dest_qpn=0x%x",
                   bk_qp->bk_qp.qpnum, qp_attr->dest_qp_num);
	return 0;
}

int vrdma_modify_backend_qp_to_rts(struct vrdma_backend_qp *bk_qp)
{
	struct ibv_qp_attr qp_attr = {0};
	struct snap_qp *sqp;
	int attr_mask;

	sqp = bk_qp->bk_qp.sqp;

	attr_mask = IBV_QP_SQ_PSN | IBV_QP_RETRY_CNT |
				IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT |
				IBV_QP_MAX_QP_RD_ATOMIC;
	qp_attr.sq_psn = 0;
	qp_attr.retry_cnt = VRDMA_BACKEND_QP_RNR_RETRY;
	qp_attr.rnr_retry = VRDMA_BACKEND_QP_RETRY_COUNT;
	qp_attr.timeout = VRDMA_BACKEND_QP_TIMEOUT;
	qp_attr.max_rd_atomic = VRDMA_QP_MAX_RD_ATOMIC;
	if (snap_vrdma_modify_bankend_qp_rtr2rts(sqp, &qp_attr, attr_mask)) {
		SPDK_ERRLOG("Failed to modify bankend QP=0x%x RTR to RTS",
		            bk_qp->bk_qp.qpnum);
		return -1;
	}
	bk_qp->qp_state = IBV_QPS_RTS;
	SPDK_NOTICELOG("Succeeded to modify bankend QP=0x%x RTR to RTS.\n"
                   "min_rnr_timer %d retry_cnt %d rnr_retry %d timeout %d\n",
	bk_qp->bk_qp.qpnum, qp_attr.min_rnr_timer,
	qp_attr.retry_cnt, qp_attr.rnr_retry, qp_attr.timeout);
	return 0;
}

void vrdma_destroy_backend_qp(struct vrdma_backend_qp **mqp)
{
    snap_vrdma_destroy_qp_helper(&(*mqp)->bk_qp);
    free((*mqp)->sq_meta_buf);
    free(*mqp);
    *mqp = NULL;
}

static void
vrdma_dummy_rx_cb(struct snap_dma_q *q, const void *data,
						uint32_t data_len, uint32_t imm_data)
{
	snap_error("VRDMA: rx cb called\n");
}

int vrdma_create_vq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe,
				struct spdk_vrdma_qp *vqp,
				struct spdk_vrdma_cq *rq_vcq,
				struct spdk_vrdma_cq *sq_vcq)
{
	struct snap_vrdma_vq_create_attr q_attr;
	uint32_t rq_buff_size, sq_buff_size, q_buff_size;
	uint32_t sq_meta_size, local_cq_size;

	q_attr.bdev = NULL;
	q_attr.pd = ctrl->pd;
	q_attr.sq_size = VRDMA_MAX_DMA_SQ_SIZE_PER_VQP;
	q_attr.rq_size = VRDMA_MAX_DMA_RQ_SIZE_PER_VQP;
	q_attr.tx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.rx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.vqpn = vqp->qp_idx;
	q_attr.rx_cb = vrdma_dummy_rx_cb;

	if (!ctrl->dpa_enabled) {
		vqp->snap_queue = ctrl->sctrl->q_ops->create(ctrl->sctrl, &q_attr);
		if (!vqp->snap_queue) {
			SPDK_ERRLOG("Failed to create qp dma queue");
			return -1;
		}
		vqp->snap_queue->ctx = vqp;
	}
	vrdma_qp_sm_init(vqp);
	vqp->rq.comm.wqebb_size =
		VRDMA_QP_WQEBB_BASE_SIZE * (aqe->req.create_qp_req.rq_wqebb_size + 1);
	vqp->rq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_rq_wqebb_cnt;
	rq_buff_size = vqp->rq.comm.wqebb_size * vqp->rq.comm.wqebb_cnt;
	q_buff_size = sizeof(*vqp->qp_pi) + sizeof(*vqp->dpa_fields) + rq_buff_size;
	vqp->sq.comm.wqebb_size =
		VRDMA_QP_WQEBB_BASE_SIZE * (aqe->req.create_qp_req.sq_wqebb_size + 1);
	vqp->sq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_sq_wqebb_cnt;
	sq_buff_size = vqp->sq.comm.wqebb_size * vqp->sq.comm.wqebb_cnt;
	q_buff_size += sq_buff_size;
	sq_meta_size = sizeof(struct vqp_sq_meta) * vqp->sq.comm.wqebb_cnt;
	q_buff_size += sq_meta_size;
	local_cq_size = sizeof(struct vrdma_cqe) * vqp->sq.comm.wqebb_cnt;
	q_buff_size += local_cq_size;

	vqp->qp_pi = spdk_malloc(q_buff_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!vqp->qp_pi) {
		SPDK_ERRLOG("Failed to allocate wqe buff");
        goto destroy_dma;
    }
	vqp->dpa_fields = (struct dpa_updated_fields *)((uint8_t *)vqp->qp_pi + sizeof(*vqp->qp_pi));
	vqp->rq.rq_buff = (struct vrdma_recv_wqe *)((uint8_t *)vqp->dpa_fields + sizeof(*vqp->dpa_fields));
	vqp->sq.sq_buff = (struct vrdma_send_wqe *)((uint8_t *)vqp->rq.rq_buff + rq_buff_size);
	vqp->sq.meta_buff = (struct vrdma_sq_meta *)((uint8_t *)vqp->sq.sq_buff + sq_buff_size);
	vqp->sq.local_cq_buff = (struct vrdma_cqe *)((uint8_t *)vqp->sq.meta_buff + sq_meta_size);

    vqp->qp_mr = ibv_reg_mr(ctrl->pd, vqp->qp_pi, q_buff_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!vqp->qp_mr) {
		SPDK_ERRLOG("Failed to register qp_mr");
        goto free_wqe_buff;
    }
	vqp->rq.comm.wqe_buff_pa = aqe->req.create_qp_req.rq_l0_paddr;
	vqp->rq.comm.doorbell_pa = aqe->req.create_qp_req.rq_pi_paddr;
	vqp->rq.comm.log_pagesize = aqe->req.create_qp_req.log_rq_pagesize;
	vqp->rq.comm.hop = aqe->req.create_qp_req.rq_hop;
	vqp->sq.comm.wqe_buff_pa = aqe->req.create_qp_req.sq_l0_paddr;
	vqp->sq.comm.doorbell_pa = aqe->req.create_qp_req.sq_pi_paddr;
	vqp->sq.comm.log_pagesize = aqe->req.create_qp_req.log_sq_pagesize;
	vqp->sq.comm.hop = aqe->req.create_qp_req.sq_hop;

	if (ctrl->dpa_enabled) {
		struct vrdma_dpa_thread_ctx *dpa_thread;
		//SPDK_NOTICELOG("===================vrdma_qp.c=================");
		//SPDK_NOTICELOG("vqp %d qdb_idx %d lkey %#x rkey %#x\n",
		//		vqp->qp_idx, vqp->qdb_idx, vqp->qp_mr->lkey, vqp->qp_mr->rkey);
		dpa_thread = vrdma_prov_thread_ctx_create(ctrl, vqp, &q_attr);
		if (dpa_thread) {
			vrdma_prov_vq_map_to_thread(ctrl, vqp, dpa_thread);
			//SPDK_NOTICELOG("vrdma_create_vq end, binded to dpa_thread %d\n",
			//				dpa_thread->thread_idx);
		} else {
			SPDK_ERRLOG("vrdma_create_vq...fail\n");
			return -1;
		}
	}
	return 0;

free_wqe_buff:
	spdk_free(vqp->qp_pi);
destroy_dma:
	if (!ctrl->dpa_enabled) {
		ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	}
	return -1;
}

bool vrdma_set_vq_flush(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
    uint16_t wqe_idx, mqp_pi, mqp_ci, q_size;
    struct mqp_sq_meta *sq_meta = NULL;
    struct vrdma_backend_qp *mqp = vqp->bk_qp;

    if (mqp) {
        q_size = mqp->bk_qp.hw_qp.sq.wqe_cnt;
        mqp_pi = mqp->bk_qp.hw_qp.sq.pi;
        mqp_ci = mqp->bk_qp.sq_ci;
        if (vrdma_vq_rollback(mqp_ci, mqp_pi, q_size)) {
            mqp_pi += q_size;
        }
        for (wqe_idx = mqp_ci; wqe_idx < mqp_pi; wqe_idx++) {
            sq_meta = &mqp->sq_meta_buf[wqe_idx & (q_size - 1)];
            if (sq_meta->vqp && sq_meta->vqp->qp_idx == vqp->qp_idx)
                sq_meta->vqp = NULL;
        }
    }
	vqp->sw_state = VRDMA_QP_SW_STATE_FLUSHING;
	return true;
}

void vrdma_destroy_vq(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	if (ctrl->sctrl) {
		if (!ctrl->dpa_enabled) {
			ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
		} else {
			vrdma_prov_vq_destroy(vqp);
		}
	}
	if (vqp->qp_mr) {
		ibv_dereg_mr(vqp->qp_mr);
		vqp->qp_mr = NULL;
	}
	if (vqp->qp_pi) {
		spdk_free(vqp->qp_pi);
		vqp->qp_pi = NULL;
		vqp->rq.rq_buff = NULL;
		vqp->sq.sq_buff = NULL;
	}
}

bool vrdma_qp_is_suspended(struct vrdma_ctrl *ctrl, uint32_t qp_handle)
{
	struct spdk_vrdma_qp *vqp;

	vqp = find_spdk_vrdma_qp_by_idx(ctrl, qp_handle);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find QP %d in waiting qp suspended progress",
			qp_handle);
		return false;
	}
	return vqp->sw_state == VRDMA_QP_SW_STATE_SUSPENDED;
}

bool vrdma_qp_is_connected_ready(struct spdk_vrdma_qp *vqp)
{
	if (vqp->qp_state > IBV_QPS_INIT && vqp->qp_state < IBV_QPS_ERR)
		return true;
	return false;
}

void vrdma_set_rpc_msg_with_mqp_info(struct vrdma_ctrl *ctrl,
                                     struct vrdma_tgid_node *tgid_node,
                                     uint8_t mqp_idx,
                                     struct spdk_vrdma_rpc_qp_msg *msg)
{
    struct vrdma_backend_qp *mqp = tgid_node->src_udp[mqp_idx].mqp;
    msg->emu_manager = ctrl->emu_manager;
    memcpy(&msg->sf_mac, &tgid_node->ctrl->vdev->vrdma_sf.mac, 6);
    msg->bk_qpn = mqp->bk_qp.qpnum;
    msg->qp_state = mqp->qp_state;
    msg->mqp_idx = mqp_idx;
    msg->next_rcv_psn = mqp->mig_ctx.mig_lnxt_rcv_psn;
    msg->local_tgid = tgid_node->key.local_tgid;
    msg->remote_tgid = tgid_node->key.remote_tgid;
    msg->local_mgid.global.interface_id = tgid_node->ctrl->vdev->vrdma_sf.ip;
    msg->local_mgid.global.subnet_prefix = 0;
    msg->remote_mgid.global.interface_id = tgid_node->ctrl->vdev->vrdma_sf.remote_ip;
    msg->remote_mgid.global.subnet_prefix = 0;
}

int vrdma_qp_notify_remote_by_rpc(struct vrdma_ctrl *ctrl,
                                  struct vrdma_tgid_node *tgid_node,
                                  uint8_t mqp_idx)
{
	struct spdk_vrdma_rpc_qp_msg msg = {0};

    vrdma_set_rpc_msg_with_mqp_info(ctrl, tgid_node, mqp_idx, &msg);
    if (spdk_vrdma_rpc_send_qp_msg(g_vrdma_rpc.node_rip, &msg)) {
        SPDK_ERRLOG("Fail to send local tgid 0x%llx to remote tgid 0x%llx\n",
            tgid_node->key.local_tgid.global.interface_id,
            tgid_node->key.remote_tgid.global.interface_id);
    }
	return 0;
}

static struct snap_vrdma_queue *
vrdma_ctrl_create_dma_qp(struct vrdma_ctrl *ctrl,
									struct spdk_vrdma_qp *vqp)

{
	struct snap_vrdma_vq_create_attr q_attr = {0};

	q_attr.bdev = NULL;
	q_attr.pd = ctrl->pd;
	q_attr.sq_size = VRDMA_MAX_DMA_SQ_SIZE_PER_VQP;
	q_attr.rq_size = VRDMA_MAX_DMA_RQ_SIZE_PER_VQP;
	q_attr.tx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.rx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.rx_cb = vrdma_dummy_rx_cb;

	q_attr.vqpn = vqp->qp_idx;
	return ctrl->sctrl->q_ops->create(ctrl->sctrl, &q_attr);
}

static struct snap_vrdma_queue *
vrdma_ctrl_find_dma_qp(struct vrdma_ctrl *ctrl,
								struct spdk_vrdma_qp *vqp,
								int pg_id)
{
	struct snap_vrdma_queue *snap_queue;

	if (!(pg_id < VRDMA_MAX_THREAD_NUM)) {
		SPDK_ERRLOG("pg id is too large %d \n",
					pg_id);
		return NULL;
	}

	if (!ctrl->sw_dma_q[pg_id]) {
		snap_queue = vrdma_ctrl_create_dma_qp(ctrl, vqp);
		if (!snap_queue) {
			SPDK_ERRLOG("Failed to create qp dma queue for thread %d \n",
						pg_id);
			return NULL;
		}
		ctrl->sw_dma_q[pg_id] = snap_queue;
	}

	return ctrl->sw_dma_q[pg_id];
}

static void
sw_dma_qp_destroy(struct snap_vrdma_queue *sw_dma_qp)
{
	if (!sw_dma_qp) {
		return;
	}
	if (sw_dma_qp->dma_q) {
		snap_dma_ep_destroy(sw_dma_qp->dma_q);
	}
	free(sw_dma_qp);
}

static int vrdma_sched_vq_nolock(struct snap_vrdma_ctrl *ctrl,
					    struct spdk_vrdma_qp *vq,
					    struct snap_pg *pg)
{
	TAILQ_INSERT_TAIL(&pg->q_list, &vq->pg_q, entry);
	vq->pg = pg;
	return 0;
}

int vrdma_sched_vq(struct snap_vrdma_ctrl *ctrl,
				     struct spdk_vrdma_qp *vq,
				     struct snap_pg *pg)
{
	struct vrdma_ctrl *v_ctrl = ctrl->cb_ctx;

	vq->snap_queue = vrdma_ctrl_find_dma_qp(v_ctrl, vq, pg->id);
	if (!vq->snap_queue) {
		SPDK_ERRLOG("VRDMA queue %d failed to join scheduler\n", vq->qp_idx);
		return -1;
	}
	pthread_spin_lock(&pg->lock);
	vrdma_sched_vq_nolock(ctrl, vq, pg);
	//SPDK_NOTICELOG("VRDMA queue id %d sched polling group id = %d\n",
	//				vq->qp_idx, vq->pg->id);
	pthread_spin_unlock(&pg->lock);
	return 0;
}

void vrdma_desched_vq_nolock(struct spdk_vrdma_qp *vq)
{
	struct snap_pg *pg = vq->pg;

	if (!pg)
		return;

	TAILQ_REMOVE(&pg->q_list, &vq->pg_q, entry);
	snap_pg_usage_decrease(vq->pg->id);
	vq->pg = NULL;
}

void vrdma_desched_vq(struct spdk_vrdma_qp *vq)
{
	struct snap_pg *pg = vq->pg;

	if (!pg)
		return;

	//SPDK_NOTICELOG("VRDMA queue id %d desched polling group id = %d\n",
	//				vq->qp_idx, vq->pg->id);
	pthread_spin_lock(&pg->lock);
	vrdma_desched_vq_nolock(vq);
	pthread_spin_unlock(&pg->lock);
}

void vrdma_ctrl_destroy_dma_qp(struct vrdma_ctrl *ctrl)

{
	uint16_t thread;

	for (thread = 0; thread < VRDMA_MAX_THREAD_NUM; thread++) {
		if (ctrl->sw_dma_q[thread]) {
			sw_dma_qp_destroy(ctrl->sw_dma_q[thread]);
		}
	}
}
