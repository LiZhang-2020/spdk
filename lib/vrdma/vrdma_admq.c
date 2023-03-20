/*-
 *   BSD LICENSE
 *
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
#include "snap.h"
#include "snap_vrdma_ctrl.h"
#include "snap_vrdma_virtq.h"

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/bit_array.h"
#include "spdk/barrier.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/likely.h"

#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_qp.h"
#include "spdk/vrdma_mr.h"

static uint32_t g_vpd_cnt;
static uint32_t g_vmr_cnt;
static uint32_t g_vah_cnt;
static uint32_t g_vqp_cnt;
static uint32_t g_vcq_cnt;
static uint32_t g_veq_cnt;

static struct spdk_bit_array *
spdk_vrdma_create_id_pool(uint32_t max_num)
{
	struct spdk_bit_array *free_ids;
	uint32_t i;

	free_ids = spdk_bit_array_create(max_num + 1);
	if (!free_ids)
		return NULL;
	for (i = 0; i <= max_num; i++)
		spdk_bit_array_clear(free_ids, i);
	return free_ids;
}

int spdk_vrdma_init_all_id_pool(struct spdk_vrdma_dev *vdev)
{
	vdev->free_vpd_ids = spdk_vrdma_create_id_pool(VRDMA_DEV_MAX_PD);
	if (!vdev->free_vpd_ids)
		return -1;
	vdev->free_vmr_ids = spdk_vrdma_create_id_pool(VRDMA_DEV_MAX_MR);
	if (!vdev->free_vmr_ids)
		goto del_vpd;
	vdev->free_vah_ids = spdk_vrdma_create_id_pool(VRDMA_DEV_MAX_AH);
	if (!vdev->free_vah_ids)
		goto del_vmr;
	vdev->free_vqp_ids = spdk_vrdma_create_id_pool(VRDMA_DEV_MAX_QP);
	if (!vdev->free_vqp_ids)
		goto del_vah;
	vdev->free_vcq_ids = spdk_vrdma_create_id_pool(VRDMA_DEV_MAX_CQ);
	if (!vdev->free_vcq_ids)
		goto del_vqp;
	vdev->free_veq_ids = spdk_vrdma_create_id_pool(VRDMA_DEV_MAX_EQ);
	if (!vdev->free_veq_ids)
		goto del_vcq;
	return 0;
del_vcq:
	spdk_bit_array_free(&vdev->free_vcq_ids);
del_vqp:
	spdk_bit_array_free(&vdev->free_vqp_ids);
del_vah:
	spdk_bit_array_free(&vdev->free_vah_ids);
del_vmr:
	spdk_bit_array_free(&vdev->free_vmr_ids);
del_vpd:
	spdk_bit_array_free(&vdev->free_vpd_ids);
	return -1;
}

int spdk_vrdma_adminq_resource_init(void)
{
	g_vpd_cnt = 0;
	g_vqp_cnt = 0;
	g_vcq_cnt = 0;
	g_veq_cnt = 0;
	g_vmr_cnt = 0;
	g_vah_cnt = 0;
	return 0;
}

struct spdk_vrdma_pd *
find_spdk_vrdma_pd_by_idx(struct vrdma_ctrl *ctrl, uint32_t pd_idx)
{
	struct spdk_vrdma_pd *vpd = NULL;

	LIST_FOREACH(vpd, &ctrl->vdev->vpd_list, entry)
        if (vpd->pd_idx == pd_idx)
            break;
	return vpd;
}

struct spdk_vrdma_mr *
find_spdk_vrdma_mr_by_idx(struct vrdma_ctrl *ctrl, uint32_t mr_idx)
{
	struct spdk_vrdma_mr *vmr = NULL;

	LIST_FOREACH(vmr, &ctrl->vdev->vmr_list, entry)
        if (vmr->mr_idx == mr_idx)
            break;
	return vmr;
}

struct spdk_vrdma_mr *
find_spdk_vrdma_mr_by_key(struct vrdma_ctrl *ctrl, uint32_t key)
{
	struct spdk_vrdma_mr *vmr = NULL;

	LIST_FOREACH(vmr, &ctrl->vdev->vmr_list, entry)
        if (vmr->mr_log.mkey == key)
            break;
	return vmr;
}

struct spdk_vrdma_ah *
find_spdk_vrdma_ah_by_idx(struct vrdma_ctrl *ctrl, uint32_t ah_idx)
{
	struct spdk_vrdma_ah *vah = NULL;

	LIST_FOREACH(vah, &ctrl->vdev->vah_list, entry)
        if (vah->ah_idx == ah_idx)
            break;
	return vah;
}

struct spdk_vrdma_cq *
find_spdk_vrdma_cq_by_idx(struct vrdma_ctrl *ctrl, uint32_t cq_idx)
{
	struct spdk_vrdma_cq *vcq = NULL;

	LIST_FOREACH(vcq, &ctrl->vdev->vcq_list, entry)
        if (vcq->cq_idx == cq_idx)
            break;
	return vcq;
}

struct spdk_vrdma_eq *
find_spdk_vrdma_eq_by_idx(struct vrdma_ctrl *ctrl, uint32_t eq_idx)
{
	struct spdk_vrdma_eq *veq = NULL;

	LIST_FOREACH(veq, &ctrl->vdev->veq_list, entry)
        if (veq->eq_idx == eq_idx)
            break;
	return veq;
}

static inline int aqe_sanity_check(struct vrdma_admin_cmd_entry *aqe)
{
	if (!aqe) {
		SPDK_ERRLOG("check input aqe NULL\n");
		return -1;
	}
	
	if (aqe->hdr.magic != VRDMA_AQ_HDR_MEGIC_NUM) { 
		SPDK_ERRLOG("check input aqe wrong megic num\n");
		return -1;
	}
	if (aqe->hdr.is_inline_out || !aqe->hdr.is_inline_in) {
		/* It only supports inline message */
		SPDK_ERRLOG("check input aqe wrong inline flag\n");
		return -1;
	}
	//TODO: add other sanity check later
	return 0;
}

static void vrdma_ctrl_dev_init(struct vrdma_ctrl *ctrl)
{
	struct snap_device *sdev = ctrl->sctrl->sdev;
	struct snap_vrdma_device_attr vattr = {};

	if (ctrl->dev_inited)
		return;
	if (snap_vrdma_query_device(sdev, &vattr))
		return;
    memcpy(ctrl->dev.gid, &vattr.mac, sizeof(uint64_t));
	memcpy(ctrl->dev.mac, &vattr.mac, sizeof(uint64_t));
	ctrl->dev.state = vattr.status;
	ctrl->dev_inited = 1;
}

static void vrdma_aq_open_dev(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	if (ctrl->srv_ops->vrdma_device_open_device(&ctrl->dev, aqe)) {
		aqe->resp.open_device_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	aqe->resp.open_device_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_query_dev(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{

	struct snap_device *sdev = ctrl->sctrl->sdev;
	const char fw_ver[] = "Unkown";

	memcpy(aqe->resp.query_device_resp.fw_ver, fw_ver, strlen(fw_ver));
	aqe->resp.query_device_resp.dev_cap_flags = VRDMA_DEVICE_RC_RNR_NAK_GEN;
	aqe->resp.query_device_resp.vendor_id = sdev->pci->pci_attr.vendor_id;
	aqe->resp.query_device_resp.hw_ver = sdev->pci->pci_attr.revision_id;
	aqe->resp.query_device_resp.max_pd = 1 << ctrl->sctx->vrdma_caps.log_max_pd;
	aqe->resp.query_device_resp.max_qp = VRDMA_DEV_MAX_QP;
	aqe->resp.query_device_resp.max_qp_wr = VRDMA_DEV_MAX_QP_SZ;
	aqe->resp.query_device_resp.max_cq = VRDMA_DEV_MAX_CQ;
	aqe->resp.query_device_resp.max_sq_depth = VRDMA_DEV_MAX_SQ_DP;
	aqe->resp.query_device_resp.max_rq_depth = VRDMA_DEV_MAX_RQ_DP;
	aqe->resp.query_device_resp.max_cq_depth = VRDMA_DEV_MAX_CQ_DP;
	aqe->resp.query_device_resp.max_mr = 1 << ctrl->sctx->vrdma_caps.log_max_mkey;
	aqe->resp.query_device_resp.max_ah = VRDMA_DEV_MAX_AH;
	if (ctrl->srv_ops->vrdma_device_query_device(&ctrl->dev, aqe)) {
		aqe->resp.query_device_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	aqe->resp.query_device_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_query_port(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	uint32_t path_mtu;

	aqe->resp.query_port_resp.state = IBV_PORT_ACTIVE; /* hardcode just for POC*/
	path_mtu = ctrl->sctrl->bar_curr->mtu;
	if (path_mtu >= 4096)
		aqe->resp.query_port_resp.max_mtu = IBV_MTU_4096;
	else if (path_mtu >= 2048)
		aqe->resp.query_port_resp.max_mtu = IBV_MTU_2048;
	else if (path_mtu >= 1024)
		aqe->resp.query_port_resp.max_mtu = IBV_MTU_1024;
	else if (path_mtu >= 512)
		aqe->resp.query_port_resp.max_mtu = IBV_MTU_512;
	else
		aqe->resp.query_port_resp.max_mtu = IBV_MTU_256;
	aqe->resp.query_port_resp.active_mtu = aqe->resp.query_port_resp.max_mtu;
	aqe->resp.query_port_resp.gid_tbl_len = 1;/* hardcode just for POC*/
	aqe->resp.query_port_resp.max_msg_sz = 1 << ctrl->sctx->vrdma_caps.log_max_msg;
	aqe->resp.query_port_resp.sm_lid = 0xFFFF; /*IB_LID_PERMISSIVE*/
	aqe->resp.query_port_resp.lid = 0xFFFF;
	aqe->resp.query_port_resp.pkey_tbl_len = 1;/* hardcode just for POC*/
	aqe->resp.query_port_resp.active_speed = 16; /* FDR hardcode just for POC*/
	aqe->resp.query_port_resp.phys_state = VRDMA_PORT_PHYS_STATE_LINK_UP;
	aqe->resp.query_port_resp.link_layer = IBV_LINK_LAYER_INFINIBAND;
	if (ctrl->srv_ops->vrdma_device_query_port(&ctrl->dev, aqe)) {
		aqe->resp.query_port_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	aqe->resp.query_port_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_query_gid(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	memcpy(aqe->resp.query_gid_resp.gid, ctrl->dev.gid, VRDMA_DEV_GID_LEN);
	if (ctrl->srv_ops->vrdma_device_query_gid(&ctrl->dev, aqe)) {
		aqe->resp.query_gid_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	aqe->resp.query_gid_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	for (int i = 0; i < VRDMA_DEV_GID_LEN; i += 4)
		SPDK_NOTICELOG("current gid=0x%x%x%x%x\n",
			aqe->resp.query_gid_resp.gid[i], aqe->resp.query_gid_resp.gid[i+1],
			aqe->resp.query_gid_resp.gid[i+2], aqe->resp.query_gid_resp.gid[i+3]);
}

static void vrdma_aq_modify_gid(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct vrdma_cmd_param param;
	
	memcpy(param.param.modify_gid_param.gid, aqe->req.modify_gid_req.gid,
			VRDMA_DEV_GID_LEN);
	if (ctrl->srv_ops->vrdma_device_modify_gid(&ctrl->dev, aqe, &param)) {
		aqe->resp.modify_gid_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		return;
	}
	memcpy(ctrl->dev.gid, aqe->req.modify_gid_req.gid, VRDMA_DEV_GID_LEN);
	aqe->resp.modify_gid_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	for (int i = 0; i < VRDMA_DEV_GID_LEN; i += 4)
		SPDK_NOTICELOG("new gid=0x%x%x%x%x\n",
			aqe->req.modify_gid_req.gid[i], aqe->req.modify_gid_req.gid[i+1],
			aqe->req.modify_gid_req.gid[i+2], aqe->req.modify_gid_req.gid[i+3]);
}

static void vrdma_aq_create_pd(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd;
	uint32_t pd_idx;

	if (g_vpd_cnt > VRDMA_MAX_PD_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vpd_cnt > VRDMA_DEV_MAX_PD) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create PD, err(%d)\n",
				  aqe->resp.create_pd_resp.err_code);
		return;
	}
	pd_idx = spdk_bit_array_find_first_clear(ctrl->vdev->free_vpd_ids, 0);
	if (pd_idx == UINT32_MAX) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate PD index, err(%d)\n",
				  aqe->resp.create_pd_resp.err_code);
		return;
	}
    vpd = calloc(1, sizeof(*vpd));
    if (!vpd) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate PD memory, err(%d)\n",
				  aqe->resp.create_pd_resp.err_code);
		return;
	}
	vpd->ibpd = ctrl->vdev->vrdma_sf.sf_pd;
	if (!vpd->ibpd) {
		aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate PD, err(%d)\n",
				  aqe->resp.create_pd_resp.err_code);
		goto free_vpd;
	}
	param.param.create_pd_param.pd_handle = pd_idx;
	if (ctrl->srv_ops->vrdma_device_create_pd(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify PD in service, err(%d)\n",
				  aqe->resp.create_pd_resp.err_code);
		goto free_vpd;
	}
	g_vpd_cnt++;
	ctrl->vdev->vpd_cnt++;
	vpd->pd_idx = pd_idx;
	spdk_bit_array_set(ctrl->vdev->free_vpd_ids, pd_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vpd_list, vpd, entry);
	aqe->resp.create_pd_resp.pd_handle = pd_idx;
	aqe->resp.create_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("pd_idx %d pd %p successfully\n", pd_idx, vpd->ibpd);
	return;

free_vpd:
	free(vpd);
}

static void vrdma_aq_destroy_pd(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_pd *vpd = NULL;

	SPDK_NOTICELOG("pd_handle %d\n", aqe->req.destroy_pd_req.pd_handle);
	if (!g_vpd_cnt || !ctrl->vdev ||
		!ctrl->vdev->vpd_cnt) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy PD, err(%d)\n",
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.destroy_pd_req.pd_handle);
	if (!vpd) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD handle %d, err(%d)\n",
				  aqe->req.destroy_pd_req.pd_handle,
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	if (vpd->ref_cnt) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("PD handle %d is used now, err(%d)\n",
				  aqe->req.destroy_pd_req.pd_handle,
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_pd(&ctrl->dev, aqe)) {
		aqe->resp.destroy_pd_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy PD handle %d in service, err(%d)\n",
				  aqe->req.destroy_pd_req.pd_handle,
				  aqe->resp.destroy_pd_resp.err_code);
		return;
	}
	LIST_REMOVE(vpd, entry);
	spdk_bit_array_clear(ctrl->vdev->free_vpd_ids, vpd->pd_idx);
    free(vpd);
	g_vpd_cnt--;
	ctrl->vdev->vpd_cnt--;
	aqe->resp.destroy_pd_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_reg_mr(struct vrdma_ctrl *ctrl,
	        	struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_mr *vmr;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd = NULL;
	uint32_t dev_max_mr = spdk_min(VRDMA_DEV_MAX_MR,
			(1 << ctrl->sctx->vrdma_caps.log_max_mkey));
	uint32_t i, mr_idx, total_len = 0;

	if (g_vmr_cnt > VRDMA_MAX_MR_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vmr_cnt > dev_max_mr) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to register MR, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	if (!aqe->req.create_mr_req.sge_count || !aqe->req.create_mr_req.length) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to register MR, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.create_mr_req.pd_handle);
	if (!vpd) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD %d when creation MR, err(%d)\n",
					aqe->req.create_mr_req.pd_handle,
					aqe->resp.create_mr_resp.err_code);
		return;
	}
	if (aqe->req.create_mr_req.sge_count > MAX_VRDMA_MR_SGE_NUM) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to register MR for sge_count more than 8, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	for (i = 0; i < aqe->req.create_mr_req.sge_count; i++) {
		total_len += aqe->req.create_mr_req.sge_list[i].length;
	}
	if (total_len < aqe->req.create_mr_req.length) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to register MR for sge length %d more than %ld, err(%d)\n",
				total_len, aqe->req.create_mr_req.length,
				aqe->resp.create_mr_resp.err_code);
		return;
	}
	mr_idx = spdk_bit_array_find_first_clear(ctrl->vdev->free_vmr_ids, VRDMA_MR_START_IDX);
	if (mr_idx == UINT32_MAX) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate mr_idx, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
    vmr = calloc(1, sizeof(*vmr));
    if (!vmr) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate MR memory, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		return;
	}
	vrdma_reg_mr_create_attr(&aqe->req.create_mr_req, vmr);
	vmr->vpd = vpd;
	if (vrdma_create_remote_mkey(ctrl, vmr)) {
		aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
		SPDK_ERRLOG("Failed to register MR remote mkey, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		goto free_vmr;
	}

	param.param.create_mr_param.mr_handle = mr_idx;
	param.param.create_mr_param.lkey = mr_idx;
	param.param.create_mr_param.rkey = mr_idx;
	if (ctrl->srv_ops->vrdma_device_create_mr(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to register MR in service, err(%d)\n",
				  aqe->resp.create_mr_resp.err_code);
		goto free_mkey;
	}
	g_vmr_cnt++;
	ctrl->vdev->vmr_cnt++;
	vmr->mr_idx = mr_idx;
	vpd->ref_cnt++;
	spdk_bit_array_set(ctrl->vdev->free_vmr_ids, mr_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vmr_list, vmr, entry);
	/*Install vkey*/
	ctrl->vdev->l_vkey_tbl.vkey[mr_idx].mkey = vmr->mr_log.mkey;
	ctrl->vdev->l_vkey_tbl.vkey[mr_idx].vpd = vpd;
	aqe->resp.create_mr_resp.rkey = mr_idx;
	aqe->resp.create_mr_resp.lkey = aqe->resp.create_mr_resp.rkey;
	aqe->resp.create_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("register MR remote mkey, pd id %d, "
			"pd %p mr_idx %d successfully\n",
			aqe->req.create_mr_req.pd_handle, vpd->ibpd, mr_idx);
	return;
free_mkey:
	vrdma_destroy_remote_mkey(ctrl, vmr);
free_vmr:
	free(vmr);
}

static void vrdma_aq_dereg_mr(struct vrdma_ctrl *ctrl,
			struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp_tmp, *vqp = NULL;
	struct spdk_vrdma_mr *vmr = NULL;
	struct vrdma_cmd_param param;

	SPDK_NOTICELOG("lkey=0x%x\n", aqe->req.destroy_mr_req.lkey);
	if (!g_vmr_cnt || !ctrl->vdev ||
		!ctrl->vdev->vmr_cnt ||
		!aqe->req.destroy_mr_req.lkey) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to dereg MR, err(%d)\n",
				  aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	vmr = find_spdk_vrdma_mr_by_idx(ctrl, aqe->req.destroy_mr_req.lkey);
	if (!vmr) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find MR %d dereg MR, err(%d)\n",
					aqe->req.destroy_mr_req.lkey,
					aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	if (vmr->ref_cnt) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("MR %d is used now, err(%d)\n",
					aqe->req.destroy_mr_req.lkey,
					aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	param.param.destroy_mr_param.mr_handle = vmr->mr_idx;
	if (ctrl->srv_ops->vrdma_device_destroy_mr(&ctrl->dev, aqe, &param)) {
		aqe->resp.destroy_mr_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify MR %d dereg MR in service, err(%d)\n",
					aqe->req.destroy_mr_req.lkey,
					aqe->resp.destroy_mr_resp.err_code);
		return;
	}
	LIST_REMOVE(vmr, entry);
	vrdma_destroy_remote_mkey(ctrl, vmr);
	ctrl->vdev->l_vkey_tbl.vkey[vmr->mr_idx].mkey = 0;
	ctrl->vdev->l_vkey_tbl.vkey[vmr->mr_idx].vpd = 0;
	LIST_FOREACH_SAFE(vqp, &ctrl->vdev->vqp_list, entry, vqp_tmp) {
        if (vqp->last_l_vkey == vmr->mr_idx) {
			vqp->last_l_vkey = 0;
			vqp->last_l_mkey = 0;
		}
	}
	spdk_bit_array_clear(ctrl->vdev->free_vmr_ids, vmr->mr_idx);
	g_vmr_cnt--;
	ctrl->vdev->vmr_cnt--;
	vmr->vpd->ref_cnt--;
	free(vmr);
	aqe->resp.destroy_mr_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

/* vqp -> mlnx_qp -> mlnx_cq-> vcq -> veq -> vector_idx -> mlnx_cqn for msix */
static void vrdma_aq_create_cq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_cq *vcq;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_eq *veq;
	uint32_t cq_idx, q_buff_size;

	if (g_vcq_cnt > VRDMA_MAX_CQ_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vcq_cnt > VRDMA_DEV_MAX_CQ) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create CQ, err(%d)\n",
				  aqe->resp.create_cq_resp.err_code);
		return;
	}
	veq = find_spdk_vrdma_eq_by_idx(ctrl, aqe->req.create_cq_req.ceq_handle);
	if (!veq) {
		aqe->resp.create_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find CEQ %d when creation CQ, err(%d)\n",
					aqe->req.create_cq_req.ceq_handle,
					aqe->resp.create_cq_resp.err_code);
		return;
	}
	cq_idx = spdk_bit_array_find_first_clear(ctrl->vdev->free_vcq_ids, 0);
	if (cq_idx == UINT32_MAX) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate cq_idx, err(%d)\n",
				  aqe->resp.create_cq_resp.err_code);
		return;
	}
    vcq = calloc(1, sizeof(*vcq));
    if (!vcq) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate CQ memory, err(%d)\n",
				  aqe->resp.create_cq_resp.err_code);
		return;
	}
	vcq->veq = veq;
	vcq->cq_idx = cq_idx;
	vcq->cqe_entry_num =  1 << aqe->req.create_cq_req.log_cqe_entry_num;
	vcq->cqebb_size =
		VRDMA_CQEBB_BASE_SIZE * (aqe->req.create_cq_req.cqebb_size + 1);
	vcq->pagesize = 1 << aqe->req.create_cq_req.log_pagesize;
	vcq->interrupt_mode = aqe->req.create_cq_req.interrupt_mode;
	vcq->host_pa = aqe->req.create_cq_req.l0_pa;
	vcq->ci_pa = aqe->req.create_cq_req.ci_pa;
	q_buff_size  = sizeof(*vcq->pici) + vcq->cqebb_size * vcq->cqe_entry_num;
	vcq->pici = spdk_malloc(q_buff_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!vcq->pici) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate cqe buff\n");
        goto free_vcq;
    }
    vcq->cqe_ci_mr = ibv_reg_mr(ctrl->pd, vcq->pici, q_buff_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!vcq->cqe_ci_mr) {
		aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to register cqe mr\n");
        goto free_cqe_buff;
    }
	vcq->cqe_buff = (uint8_t *)vcq->pici + sizeof(*vcq->pici);
	param.param.create_cq_param.cq_handle = cq_idx;
	if (ctrl->srv_ops->vrdma_device_create_cq(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create CQ in service, err(%d)\n",
				  aqe->resp.create_cq_resp.err_code);
		goto free_cqe_mr;
	}
	g_vcq_cnt++;
	ctrl->vdev->vcq_cnt++;
	veq->ref_cnt++;
	spdk_bit_array_set(ctrl->vdev->free_vcq_ids, cq_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vcq_list, vcq, entry);
	aqe->resp.create_cq_resp.cq_handle = cq_idx;
	aqe->resp.create_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("eq_idx %d cq_idx %d successfully\n",
		aqe->req.create_cq_req.ceq_handle, cq_idx);
	return;
free_cqe_mr:
	ibv_dereg_mr(vcq->cqe_ci_mr);
free_cqe_buff:
	spdk_free(vcq->pici);
free_vcq:
	free(vcq);
}

static void vrdma_aq_destroy_cq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_cq *vcq = NULL;

	SPDK_NOTICELOG("cq_handle=0x%x\n", aqe->req.destroy_cq_req.cq_handle);
	if (!g_vcq_cnt || !ctrl->vdev ||
		!ctrl->vdev->vcq_cnt) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy CQ, err(%d)\n",
				  aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	LIST_FOREACH(vcq, &ctrl->vdev->vcq_list, entry)
        if (vcq->cq_idx == aqe->req.destroy_cq_req.cq_handle)
            break;
	if (!vcq) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy CQ %d , err(%d)\n",
					aqe->req.destroy_cq_req.cq_handle,
					aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	if (vcq->ref_cnt) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("CQ %d is used now, err(%d)\n",
					aqe->req.destroy_cq_req.cq_handle,
					aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_cq(&ctrl->dev, aqe)) {
		aqe->resp.destroy_cq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy CQ %d in service, err(%d)\n",
					aqe->req.destroy_cq_req.cq_handle,
					aqe->resp.destroy_cq_resp.err_code);
		return;
	}
	LIST_REMOVE(vcq, entry);
	spdk_bit_array_clear(ctrl->vdev->free_vcq_ids, vcq->cq_idx);
	g_vcq_cnt--;
	ctrl->vdev->vcq_cnt--;
	vcq->veq->ref_cnt--;
	ibv_dereg_mr(vcq->cqe_ci_mr);
	spdk_free(vcq->pici);
	free(vcq);
	aqe->resp.destroy_cq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_create_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd;
	struct spdk_vrdma_cq *sq_vcq;
	struct spdk_vrdma_cq *rq_vcq;
	uint32_t qp_idx;

	if (g_vqp_cnt > VRDMA_MAX_QP_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vqp_cnt > VRDMA_DEV_MAX_QP ||
		aqe->req.create_qp_req.log_sq_wqebb_cnt > VRDMA_MAX_LOG_SQ_WQEBB_CNT) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create QP, err(%d)\n",
				  aqe->resp.create_qp_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.create_qp_req.pd_handle);
	if (!vpd) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD %d when creation QP, err(%d)\n",
					aqe->req.create_qp_req.pd_handle,
					aqe->resp.create_qp_resp.err_code);
		return;
	}
	sq_vcq = find_spdk_vrdma_cq_by_idx(ctrl, aqe->req.create_qp_req.sq_cqn);
	if (!sq_vcq) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find SQ CQ %d when creation QP, err(%d)\n",
					aqe->req.create_qp_req.sq_cqn,
					aqe->resp.create_qp_resp.err_code);
		return;
	}
	rq_vcq = find_spdk_vrdma_cq_by_idx(ctrl, aqe->req.create_qp_req.rq_cqn);
	if (!rq_vcq) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find RQ CQ %d when creation QP, err(%d)\n",
					aqe->req.create_qp_req.rq_cqn,
					aqe->resp.create_qp_resp.err_code);
		return;
	}
	qp_idx = spdk_bit_array_find_first_clear(ctrl->vdev->free_vqp_ids,
			VRDMA_NORMAL_VQP_START_IDX);
	if (qp_idx == UINT32_MAX) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate qp_idx, err(%d)\n",
				  aqe->resp.create_qp_resp.err_code);
		return;
	}
    vqp = calloc(1, sizeof(*vqp));
    if (!vqp) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate QP memory, err(%d)\n",
				  aqe->resp.create_qp_resp.err_code);
		return;
	}
	SPDK_NOTICELOG("create vqp pd %p \n", vpd->ibpd);
	vqp->vpd = vpd;
	vqp->sq_vcq = sq_vcq;
	vqp->rq_vcq = rq_vcq;
	vqp->qp_idx = qp_idx;
	vqp->qdb_idx = aqe->req.create_qp_req.qdb_idx;
	vqp->l_vkey_tbl = &ctrl->vdev->l_vkey_tbl;
	if (vrdma_create_vq(ctrl, aqe, vqp, rq_vcq, sq_vcq)) {
		aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
		goto free_vqp;
	}
	param.param.create_qp_param.qp_handle = qp_idx;
	param.param.create_qp_param.ibpd = vpd->ibpd;
	if (ctrl->srv_ops->vrdma_device_create_qp(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create QP in service, err(%d)\n",
				  aqe->resp.create_qp_resp.err_code);
		goto destroy_vq;
	}
	g_vqp_cnt++;
	ctrl->vdev->vqp_cnt++;
	sq_vcq->ref_cnt++;
	rq_vcq->ref_cnt++;
	vpd->ref_cnt++;
	spdk_bit_array_set(ctrl->vdev->free_vqp_ids, qp_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vqp_list, vqp, entry);
	aqe->resp.create_qp_resp.qp_handle = qp_idx;
	aqe->resp.create_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("pd %d qp_idx %d qdb_idx %d successfully\n",
		aqe->req.create_qp_req.pd_handle, qp_idx, vqp->qdb_idx);
	return;
destroy_vq:
	vrdma_destroy_vq(ctrl, vqp);
free_vqp:
	free(vqp);
}

static void vrdma_aq_destroy_suspended_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe,
				struct spdk_vrdma_qp *in_vqp)
{
	struct spdk_vrdma_qp *vqp = in_vqp;

	SPDK_NOTICELOG("qp_handle=0x%x\n", aqe->req.destroy_qp_req.qp_handle);
	if (!vqp)
		vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.destroy_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy QP %d , err(%d)\n",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	if (vqp->ref_cnt) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("QP %d is used now, err(%d)\n",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	if (vrdma_qp_is_connected_ready(vqp) && ctrl->sctrl) {
		snap_vrdma_desched_vq(vqp->snap_queue);
	}
	vrdma_destroy_vq(ctrl, vqp);
	if (ctrl->srv_ops->vrdma_device_destroy_qp(&ctrl->dev, aqe)) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy QP %d in service, err(%d)\n",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return;
	}
	LIST_REMOVE(vqp, entry);
	spdk_bit_array_clear(ctrl->vdev->free_vqp_ids, vqp->qp_idx);
	g_vqp_cnt--;
	ctrl->vdev->vqp_cnt--;
	vqp->sq_vcq->ref_cnt--;
	vqp->rq_vcq->ref_cnt--;
	vqp->vpd->ref_cnt--;
	free(vqp);
	aqe->resp.destroy_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static int vrdma_aq_destroy_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp = NULL;

	SPDK_NOTICELOG("qp_handle=0x%x g_vqp_cnt %d vdev %p vqp_cnt %d\n",
	aqe->req.destroy_qp_req.qp_handle, g_vqp_cnt, ctrl->vdev, ctrl->vdev->vqp_cnt);
	if (!g_vqp_cnt || !ctrl->vdev ||
		!ctrl->vdev->vqp_cnt) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy QP, err(%d)",
				  aqe->resp.destroy_qp_resp.err_code);
		return 0;
	}
	vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.destroy_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy QP %d , err(%d)",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return 0;
	}
	if (vqp->ref_cnt) {
		aqe->resp.destroy_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("QP %d is used now, err(%d)",
					aqe->req.destroy_qp_req.qp_handle,
					aqe->resp.destroy_qp_resp.err_code);
		return 0;
	}
	if (vrdma_qp_is_connected_ready(vqp)) {
		if (vrdma_set_vq_flush(ctrl, vqp)) {
			return VRDMA_CMD_STATE_WAITING;
		}
	}
	vrdma_aq_destroy_suspended_qp(ctrl, aqe, vqp);
	return 0;
}

static void vrdma_aq_query_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp = NULL;

	SPDK_NOTICELOG("qp_handle=0x%x\n", aqe->req.query_qp_req.qp_handle);
	if (!g_vqp_cnt || !ctrl->vdev ||
		!ctrl->vdev->vqp_cnt) {
		aqe->resp.query_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to query QP, err(%d)\n",
				  aqe->resp.query_qp_resp.err_code);
		return;
	}
	vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.query_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.query_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find query QP %d , err(%d)\n",
					aqe->req.query_qp_req.qp_handle,
					aqe->resp.query_qp_resp.err_code);
		return;
	}
	aqe->resp.query_qp_resp.qp_state = vqp->qp_state;
	aqe->resp.query_qp_resp.rq_psn = vqp->rq_psn;
	aqe->resp.query_qp_resp.sq_psn = vqp->sq_psn;
	aqe->resp.query_qp_resp.dest_qp_num = vqp->dest_qp_num;
	aqe->resp.query_qp_resp.sq_draining = vqp->sq_draining;
	aqe->resp.query_qp_resp.qkey = vqp->qkey;
	aqe->resp.query_qp_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	
	if (ctrl->srv_ops->vrdma_device_query_qp(&ctrl->dev, aqe)) {
		aqe->resp.query_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify query QP %d in service, err(%d)\n",
					aqe->req.query_qp_req.qp_handle,
					aqe->resp.query_qp_resp.err_code);
		return;
	}
}

static void vrdma_aq_modify_qp(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_qp *vqp = NULL;

	SPDK_NOTICELOG("vqp %d qp_attr_mask = 0x%x\n",
				aqe->req.modify_qp_req.qp_handle,
				aqe->req.modify_qp_req.qp_attr_mask);
	if (!g_vqp_cnt || !ctrl->vdev ||
		!ctrl->vdev->vqp_cnt) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to modify QP, err(%d)\n",
				  aqe->resp.modify_qp_resp.err_code);
		return;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & ~vrdma_supported_qp_attr_mask) {
		SPDK_WARNLOG("Current qp_attr_mask 0x%x is some bits unsupportted"
				" in supported_qp_attr_mask 0x%x for modification qp. \n",
				vrdma_supported_qp_attr_mask, aqe->req.modify_qp_req.qp_attr_mask);
	}
	vqp = find_spdk_vrdma_qp_by_idx(ctrl,
				aqe->req.query_qp_req.qp_handle);
	if (!vqp) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find QP %d in modify progress , err(%d)\n",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
		return;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_RQ_PSN){
		vqp->rq_psn = aqe->req.modify_qp_req.rq_psn;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_SQ_PSN){
		vqp->sq_psn = aqe->req.modify_qp_req.sq_psn;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_DEST_QPN){
		vqp->dest_qp_num = aqe->req.modify_qp_req.dest_qp_num;
		vqp->remote_gid_ip = ctrl->vdev->vrdma_sf.remote_ip;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_AV){
		vqp->sip = aqe->req.modify_qp_req.sip;
		vqp->dip = aqe->req.modify_qp_req.dip;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_QKEY){
		vqp->qkey = aqe->req.modify_qp_req.qkey;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_TIMEOUT){
		vqp->timeout = aqe->req.modify_qp_req.timeout;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_MIN_RNR_TIMER){
		vqp->min_rnr_timer = aqe->req.modify_qp_req.min_rnr_timer;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_RETRY_CNT){
		vqp->timeout_retry_cnt = aqe->req.modify_qp_req.timeout_retry_cnt;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_RNR_RETRY){
		vqp->rnr_retry_cnt = aqe->req.modify_qp_req.rnr_retry_cnt;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_ACCESS_FLAGS){
		vqp->qp_access_flags = aqe->req.modify_qp_req.qp_access_flags;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_PATH_MTU){
		vqp->path_mtu = aqe->req.modify_qp_req.path_mtu;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_PKEY_INDEX){
		vqp->pkey_index = aqe->req.modify_qp_req.pkey_index;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_PORT){
		vqp->port_num = aqe->req.modify_qp_req.port_num;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_MAX_QP_RD_ATOMIC){
		vqp->max_rd_atomic = aqe->req.modify_qp_req.max_rd_atomic;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_MAX_DEST_RD_ATOMIC){
		vqp->max_dest_rd_atomic = aqe->req.modify_qp_req.max_dest_rd_atomic;
	}
	if (aqe->req.modify_qp_req.qp_attr_mask & IBV_QP_STATE){
		SPDK_NOTICELOG("vqp %d qp_state=0x%x new qp_state = 0x%x\n",
				aqe->req.modify_qp_req.qp_handle,
				vqp->qp_state, aqe->req.modify_qp_req.qp_state);
		if (vqp->qp_state == IBV_QPS_INIT &&
			aqe->req.modify_qp_req.qp_state == IBV_QPS_RTR) {
			/* init2rtr vqp join poller-group */
			snap_vrdma_sched_vq(ctrl->sctrl, vqp->snap_queue);
			vrdma_qp_sm_start(vqp);
		}
		if (vqp->qp_state != IBV_QPS_ERR &&
			aqe->req.modify_qp_req.qp_state == IBV_QPS_ERR) {
			/* Take vqp out of poller-group when it changed to ERR state */
			snap_vrdma_desched_vq(vqp->snap_queue);
		}
		vqp->qp_state = aqe->req.modify_qp_req.qp_state;
	}
	if (ctrl->srv_ops->vrdma_device_modify_qp(&ctrl->dev, aqe)) {
		aqe->resp.modify_qp_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify modify QP %d in service, err(%d)\n",
					aqe->req.modify_qp_req.qp_handle,
					aqe->resp.modify_qp_resp.err_code);
		return;
	}
}

static void vrdma_aq_create_ceq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_eq *veq;
	struct vrdma_cmd_param param;
	uint32_t eq_idx;

	if (g_veq_cnt > VRDMA_MAX_EQ_NUM ||
		!ctrl->vdev || !ctrl->sctrl ||
		ctrl->vdev->veq_cnt > VRDMA_DEV_MAX_EQ ||
		aqe->req.create_ceq_req.vector_idx >=
			ctrl->sctrl->bar_curr->num_msix) {
		aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create ceq, err(%d)\n",
				  aqe->resp.create_ceq_resp.err_code);
		return;
	}
	eq_idx = spdk_bit_array_find_first_clear(ctrl->vdev->free_veq_ids, 0);
	if (eq_idx == UINT32_MAX) {
		aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate eq_idx, err(%d)\n",
				  aqe->resp.create_ceq_resp.err_code);
		return;
	}
    veq = calloc(1, sizeof(*veq));
    if (!veq) {
		aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate CEQ memory, err(%d)\n",
				  aqe->resp.create_ceq_resp.err_code);
		return;
	}
	param.param.create_eq_param.eq_handle = eq_idx;
	if (ctrl->srv_ops->vrdma_device_create_eq(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create CEQ in service, err(%d)\n",
				  aqe->resp.create_ceq_resp.err_code);
		goto free_veq;
	}
	g_veq_cnt++;
	ctrl->vdev->veq_cnt++;
	veq->eq_idx = eq_idx;
	veq->log_depth = aqe->req.create_ceq_req.log_depth;
	veq->queue_addr = aqe->req.create_ceq_req.queue_addr;
	veq->vector_idx = aqe->req.create_ceq_req.vector_idx;
	spdk_bit_array_set(ctrl->vdev->free_veq_ids, eq_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->veq_list, veq, entry);
	aqe->resp.create_ceq_resp.ceq_handle = eq_idx;
	aqe->resp.create_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("eq_idx %d vector_idx 0x%x successfully\n",
			eq_idx, veq->vector_idx);
	return;
free_veq:
	free(veq);
}

static void vrdma_aq_modify_ceq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	aqe->resp.modify_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_UNKNOWN;
	SPDK_ERRLOG("\nIt does not support modify ceq, err(%d) \n",
				  aqe->resp.modify_ceq_resp.err_code);
}

static void vrdma_aq_destroy_ceq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_eq *veq = NULL;

	SPDK_NOTICELOG("ceq_handle=0x%x\n", aqe->req.destroy_ceq_req.ceq_handle);
	if (!g_veq_cnt || !ctrl->vdev ||
		!ctrl->vdev->veq_cnt) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy CEQ, err(%d)\n",
				  aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	veq = find_spdk_vrdma_eq_by_idx(ctrl,
				aqe->req.destroy_ceq_req.ceq_handle);
	if (!veq) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy CEQ %d , err(%d)\n",
					aqe->req.destroy_ceq_req.ceq_handle,
					aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	if (veq->ref_cnt) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("CEQ %d is used now, err(%d)\n",
					aqe->req.destroy_ceq_req.ceq_handle,
					aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_eq(&ctrl->dev, aqe)) {
		aqe->resp.destroy_ceq_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy CEQ %d in service, err(%d)\n",
					aqe->req.destroy_ceq_req.ceq_handle,
					aqe->resp.destroy_ceq_resp.err_code);
		return;
	}
	LIST_REMOVE(veq, entry);
	spdk_bit_array_clear(ctrl->vdev->free_veq_ids, veq->eq_idx);
	g_veq_cnt--;
	ctrl->vdev->veq_cnt--;
	free(veq);
	aqe->resp.destroy_ceq_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

static void vrdma_aq_create_ah(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_ah *vah;
	struct vrdma_cmd_param param;
	struct spdk_vrdma_pd *vpd = NULL;
	uint32_t ah_idx;

	if (g_vah_cnt > VRDMA_MAX_AH_NUM ||
		!ctrl->vdev ||
		ctrl->vdev->vah_cnt > VRDMA_DEV_MAX_AH) {
		aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_EXCEED_MAX;
		SPDK_ERRLOG("Failed to create ah, err(%d)\n",
				  aqe->resp.create_ah_resp.err_code);
		return;
	}
	vpd = find_spdk_vrdma_pd_by_idx(ctrl, aqe->req.create_ah_req.pd_handle);
	if (!vpd) {
		aqe->resp.create_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find PD %d when creation AH, err(%d)\n",
					aqe->req.create_ah_req.pd_handle,
					aqe->resp.create_ah_resp.err_code);
		return;
	}
	ah_idx = spdk_bit_array_find_first_clear(ctrl->vdev->free_vah_ids, 0);
	if (ah_idx == UINT32_MAX) {
		aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate ah_idx, err(%d)\n",
				  aqe->resp.create_ah_resp.err_code);
		return;
	}
    vah = calloc(1, sizeof(*vah));
    if (!vah) {
		aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_NO_MEM;
		SPDK_ERRLOG("Failed to allocate AH memory, err(%d)\n",
				  aqe->resp.create_ah_resp.err_code);
		return;
	}
	param.param.create_ah_param.ah_handle = ah_idx;
	if (ctrl->srv_ops->vrdma_device_create_ah(&ctrl->dev, aqe, &param)) {
		aqe->resp.create_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to create AH in service, err(%d)\n",
				  aqe->resp.create_ah_resp.err_code);
		goto free_vah;
	}
	g_vah_cnt++;
	ctrl->vdev->vah_cnt++;
	vah->vpd = vpd;
	vah->ah_idx = ah_idx;
	vah->dip = aqe->req.create_ah_req.dip;
	vpd->ref_cnt++;
	spdk_bit_array_set(ctrl->vdev->free_vah_ids, ah_idx);
	LIST_INSERT_HEAD(&ctrl->vdev->vah_list, vah, entry);
	aqe->resp.create_ah_resp.ah_handle = ah_idx;
	aqe->resp.create_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
	SPDK_NOTICELOG("ah_idx %d dip 0x%x successfully\n", ah_idx, vah->dip);
	return;
free_vah:
	free(vah);
}

static void vrdma_aq_destroy_ah(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe)
{
	struct spdk_vrdma_ah *vah = NULL;

	SPDK_NOTICELOG("ah_handle=0x%x\n", aqe->req.destroy_ah_req.ah_handle);
	if (!g_vah_cnt || !ctrl->vdev ||
		!ctrl->vdev->vah_cnt) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to destroy AH, err(%d)\n",
				  aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	vah = find_spdk_vrdma_ah_by_idx(ctrl, aqe->req.destroy_ah_req.ah_handle);
	if (!vah) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_INVALID_PARAM;
		SPDK_ERRLOG("Failed to find destroy AH %d , err(%d)\n",
					aqe->req.destroy_ah_req.ah_handle,
					aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	if (vah->ref_cnt) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_REF_CNT_INVALID;
		SPDK_ERRLOG("AH %d is used now, err(%d)\n",
					aqe->req.destroy_ah_req.ah_handle,
					aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	if (ctrl->srv_ops->vrdma_device_destroy_ah(&ctrl->dev, aqe)) {
		aqe->resp.destroy_ah_resp.err_code =
				VRDMA_AQ_MSG_ERR_CODE_SERVICE_FAIL;
		SPDK_ERRLOG("Failed to notify destroy AH %d in service, err(%d)\n",
					aqe->req.destroy_ah_req.ah_handle,
					aqe->resp.destroy_ah_resp.err_code);
		return;
	}
	LIST_REMOVE(vah, entry);
	spdk_bit_array_clear(ctrl->vdev->free_vah_ids, vah->ah_idx);
	g_vah_cnt--;
	ctrl->vdev->vah_cnt--;
	vah->vpd->ref_cnt--;
	free(vah);
	aqe->resp.destroy_ah_resp.err_code = VRDMA_AQ_MSG_ERR_CODE_SUCCESS;
}

int vrdma_parse_admq_entry(struct vrdma_ctrl *ctrl,
			struct vrdma_admin_cmd_entry *aqe)
{
	int ret = 0;

	if (!ctrl || aqe_sanity_check(aqe)) {
		return -1;
	}

	SPDK_NOTICELOG("entry opcode %d\n", aqe->hdr.opcode);
	switch (aqe->hdr.opcode) {
			case VRDMA_ADMIN_OPEN_DEVICE:
				vrdma_aq_open_dev(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_DEVICE:
				vrdma_aq_query_dev(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_PORT:
				vrdma_aq_query_port(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_GID:
				vrdma_aq_query_gid(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_GID:
				vrdma_aq_modify_gid(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_PD:
				vrdma_aq_create_pd(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_PD:
				vrdma_aq_destroy_pd(ctrl, aqe);
				break;
			case VRDMA_ADMIN_REG_MR:
				vrdma_aq_reg_mr(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DEREG_MR:
				vrdma_aq_dereg_mr(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_CQ:
				vrdma_aq_create_cq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_CQ:
				vrdma_aq_destroy_cq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_QP:
				vrdma_aq_create_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_QP:
				ret = vrdma_aq_destroy_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_QUERY_QP:
				vrdma_aq_query_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_QP:
				vrdma_aq_modify_qp(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_CEQ:
				vrdma_aq_create_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_MODIFY_CEQ:
				vrdma_aq_modify_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_CEQ:
				vrdma_aq_destroy_ceq(ctrl, aqe);
				break;
			case VRDMA_ADMIN_CREATE_AH:
				vrdma_aq_create_ah(ctrl, aqe);
				break;
			case VRDMA_ADMIN_DESTROY_AH:
				vrdma_aq_destroy_ah(ctrl, aqe);
				break;
			default:
				return -1;		
	}
	return ret;
}

//need invoker to guarantee pi is bigger than ci 
static inline int vrdma_aq_rollback(struct vrdma_admin_sw_qp *aq, uint16_t pi,
                                           uint16_t q_size)
{
	if (spdk_unlikely(pi % q_size == 0)) {
		return 0;
	}
	return !(pi % q_size > aq->admq->ci % q_size);
}

static bool vrdma_aq_sm_idle(struct vrdma_admin_sw_qp *aq,
                                    enum vrdma_aq_cmd_sm_op_status status)
{
	SPDK_ERRLOG("vrdma admq in invalid state %d\n",
					   VRDMA_CMD_STATE_IDLE);
	return false;
}

static bool vrdma_aq_sm_read_pi(struct vrdma_admin_sw_qp *aq,
                                   enum vrdma_aq_cmd_sm_op_status status)
{
	int ret;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint64_t pi_addr = ctrl->sctrl->bar_curr->adminq_base_addr + offsetof(struct vrdma_admin_queue, pi);

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to update admq CI, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	//SPDK_NOTICELOG("vrdam poll admin pi: admq pa 0x%lx\n", ctrl->sctrl->bar_curr->adminq_base_addr);

	aq->state = VRDMA_CMD_STATE_HANDLE_PI;
	aq->poll_comp.count = 1;

	ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, &aq->admq->pi, sizeof(uint16_t),
				          ctrl->sctrl->adminq_mr->lkey, pi_addr,
				          ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to read admin PI, ret %d\n", ret);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
	}

	return false;
}

static bool vrdma_aq_sm_handle_pi(struct vrdma_admin_sw_qp *aq,
                                    enum vrdma_aq_cmd_sm_op_status status)
{

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to get admq PI, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return false;
	}

	if (aq->admq->pi != aq->admq->ci) {
		aq->state = VRDMA_CMD_STATE_READ_CMD_ENTRY;
	} else {
		aq->state = VRDMA_CMD_STATE_POLL_PI;
	}

	return true;
}

static bool vrdma_aq_sm_read_cmd(struct vrdma_admin_sw_qp *aq,
                                    enum vrdma_aq_cmd_sm_op_status status)
{
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint16_t pi = aq->admq->pi;
	uint32_t aq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = ctrl->sctrl->bar_curr->adminq_size;
	int ret;

	host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
		             offsetof(struct vrdma_admin_queue, ring);

	aq->state = VRDMA_CMD_STATE_PARSE_CMD_ENTRY;
	aq->num_to_parse = pi - aq->admq->ci;

	SPDK_NOTICELOG("vrdam poll admin cmd: admq pa 0x%lx, pi %d, ci %d\n",
			ctrl->sctrl->bar_curr->adminq_base_addr, pi, aq->admq->ci);

	//fetch the delta PI number entry in one time
	if (!vrdma_aq_rollback(aq, pi, q_size)) {
		aq->poll_comp.count = 1;
		num = pi - aq->admq->ci;
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		offset = (aq->admq->ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
	    host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to read admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		    return true;
		}
	} else {
		/* aq roll back case, first part */
		aq->poll_comp.count = 1;
		num = q_size - (aq->admq->ci % q_size);
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		offset = (aq->admq->ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to first read admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}

		/* calculate second poll size */
		local_ring_addr = (uint8_t *)aq->admq->ring + num * sizeof(struct vrdma_admin_cmd_entry);
		aq->poll_comp.count++;
		num = pi % q_size;
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
						offsetof(struct vrdma_admin_queue, ring);
		ret = snap_dma_q_read(ctrl->sctrl->adminq_dma_q, local_ring_addr, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second read admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}
	}

	return false;
}

static bool vrdma_aq_sm_parse_cmd(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	uint16_t i;
	int ret = 0;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to get admq cmd entry, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	vrdma_ctrl_dev_init(ctrl);
    aq->state = VRDMA_CMD_STATE_WRITE_CMD_BACK;
	aq->num_parsed = aq->num_to_parse;
	for (i = 0; i < aq->num_to_parse; i++) {
		ret = vrdma_parse_admq_entry(ctrl, &(aq->admq->ring[i]));
		if (ret) {
			if (ret == VRDMA_CMD_STATE_WAITING) {
				aq->state = VRDMA_CMD_STATE_WAITING;
			}
			/* i is counted from 0, so need to add 1 */
			aq->num_parsed = i + 1;
			break;
		}
	}
	return true;
}

static bool vrdma_aq_sm_waiting(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	uint16_t wait_idx;
	struct vrdma_admin_cmd_entry *aqe;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to get admq cmd entry, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	wait_idx = aq->num_parsed - 1;
	aqe = &aq->admq->ring[wait_idx];
	if (aqe->hdr.opcode == VRDMA_ADMIN_DESTROY_QP) {
		if (vrdma_qp_is_suspended(ctrl, aqe->req.destroy_qp_req.qp_handle)) {
			vrdma_aq_destroy_suspended_qp(ctrl, aqe, NULL);
			goto next_sm;
		} else {
			aq->state = VRDMA_CMD_STATE_WAITING;
			return false;
		}
	}

next_sm:
    aq->state = VRDMA_CMD_STATE_WRITE_CMD_BACK;
	return true;
}


static bool vrdma_aq_sm_write_cmd(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint16_t num_to_write = aq->num_parsed;
	uint16_t ci = aq->admq->ci;
	uint32_t aq_poll_size = 0;
	uint64_t host_ring_addr;
	uint8_t *local_ring_addr;
	uint32_t offset = 0;
	uint16_t num = 0;
	uint16_t q_size = ctrl->sctrl->bar_curr->adminq_size;
	int ret;

	host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
		             offsetof(struct vrdma_admin_queue, ring);
	SPDK_NOTICELOG("vrdam write admin cmd: admq pa 0x%lx, num_to_write %d, old ci %d, pi %d\n", 
			ctrl->sctrl->bar_curr->adminq_base_addr, num_to_write, ci, aq->admq->pi);

	if (!num_to_write) {
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		SPDK_ERRLOG("no new aqe need to be written back, admq will be stuck!!!\n");
		return true;
	}
	aq->state = VRDMA_CMD_STATE_UPDATE_CI;

	//write back entries in one time
	if ((num_to_write + ci % q_size) > q_size) {
		/* aq roll back case, first part */
		aq->poll_comp.count = 1;
		num = q_size - (ci % q_size);
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		offset = (ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to first write admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}

		/* calculate second poll size */
		local_ring_addr = (uint8_t *)aq->admq->ring + num * sizeof(struct vrdma_admin_cmd_entry);
		aq->poll_comp.count++;
		num = num_to_write - num;
		aq_poll_size = num * sizeof(struct vrdma_admin_cmd_entry);
		host_ring_addr = ctrl->sctrl->bar_curr->adminq_base_addr +
						offsetof(struct vrdma_admin_queue, ring);
		ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, local_ring_addr, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("roll back failed to second write admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
			return true;
		}
	} else {
		aq->poll_comp.count = 1;
		aq_poll_size = num_to_write * sizeof(struct vrdma_admin_cmd_entry);
		offset = (ci % q_size) * sizeof(struct vrdma_admin_cmd_entry);
	    host_ring_addr = host_ring_addr + offset;
		ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, aq->admq->ring, aq_poll_size,
				              ctrl->sctrl->adminq_mr->lkey, host_ring_addr,
				              ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
		if (spdk_unlikely(ret)) {
			SPDK_ERRLOG("no roll back failed to write back admin CMD entry, ret %d\n", ret);
		    aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		    return true;
		}
	}

	aq->admq->ci += num_to_write;
	return false;
}

static bool vrdma_aq_sm_update_ci(struct vrdma_admin_sw_qp *aq,
                                           enum vrdma_aq_cmd_sm_op_status status)
{
	int ret;
	struct vrdma_ctrl *ctrl = container_of(aq, struct vrdma_ctrl, sw_qp);
	uint64_t ci_addr  = ctrl->sctrl->bar_curr->adminq_base_addr +
					offsetof(struct vrdma_admin_queue, ci);

	if (status != VRDMA_CMD_SM_OP_OK) {
		SPDK_ERRLOG("failed to write back admq, status %d\n", status);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
		return true;
	}

	SPDK_NOTICELOG("vrdam update admq CI: admq pa 0x%lx, new ci %d\n",
					ctrl->sctrl->bar_curr->adminq_base_addr, aq->admq->ci);

	aq->state = VRDMA_CMD_STATE_POLL_PI;
	aq->poll_comp.count = 1;
	ret = snap_dma_q_write(ctrl->sctrl->adminq_dma_q, &aq->admq->ci, sizeof(uint16_t),
				          ctrl->sctrl->adminq_mr->lkey, ci_addr,
				          ctrl->sctrl->xmkey->mkey, &aq->poll_comp);
	if (spdk_unlikely(ret)) {
		SPDK_ERRLOG("failed to update admq CI, ret %d\n", ret);
		aq->state = VRDMA_CMD_STATE_FATAL_ERR;
	}

	return false;
}

static bool vrdma_aq_sm_fatal_error(struct vrdma_admin_sw_qp *aq,
                                       enum vrdma_aq_cmd_sm_op_status status)
{
	/*
	 * TODO: maybe need to add more handling
	 */

	return false;
}

//sm array states must be according to the order of vrdma_aq_cmd_sm_state
static struct vrdma_aq_sm_state vrdma_aq_sm_arr[] = {
/*VRDMA_CMD_STATE_IDLE					*/	{vrdma_aq_sm_idle},
/*VRDMA_CMD_STATE_INIT_CI		        */	{vrdma_aq_sm_idle},
/*VRDMA_CMD_STATE_POLL_PI			    */	{vrdma_aq_sm_read_pi},
/*VRDMA_CMD_STATE_HANDLE_PI			    */	{vrdma_aq_sm_handle_pi},
/*VRDMA_CMD_STATE_READ_CMD_ENTRY	    */	{vrdma_aq_sm_read_cmd},
/*VRDMA_CMD_STATE_PARSE_CMD_ENTRY	    */	{vrdma_aq_sm_parse_cmd},
/*VRDMA_CMD_STATE_WAITING       	    */	{vrdma_aq_sm_waiting},
/*VRDMA_CMD_STATE_WRITE_CMD_BACK	    */	{vrdma_aq_sm_write_cmd},
/*VRDMA_CMD_STATE_UPDATE_CI	            */	{vrdma_aq_sm_update_ci},
/*VIRTQ_CMD_STATE_FATAL_ERR				*/	{vrdma_aq_sm_fatal_error},
											};

struct vrdma_state_machine vrdma_sm  = { vrdma_aq_sm_arr, sizeof(vrdma_aq_sm_arr) / sizeof(struct vrdma_aq_sm_state) };

/**
 * vrdma_aq_cmd_progress() - admq commanda_adminq_init state machine progress handle
 * @aq:	admq to be processed
 * @status:	status of calling function (can be a callback)
 *
 * Return: 0 (Currently no option to fail)
 */
static int vrdma_aq_cmd_progress(struct vrdma_admin_sw_qp *aq,
		          enum vrdma_aq_cmd_sm_op_status status)
{
	struct vrdma_state_machine *sm;
	bool repeat = true;

	while (repeat) {
		repeat = false;
		//SPDK_NOTICELOG("vrdma admq cmd sm state: %d\n", aq->state);
		sm = aq->custom_sm;
		if (spdk_likely(aq->state < VRDMA_CMD_NUM_OF_STATES))
			repeat = sm->sm_array[aq->state].sm_handler(aq, status);
		else
			SPDK_ERRLOG("reached invalid state %d\n", aq->state);
	}

	return 0;
}

void vrdma_aq_sm_dma_cb(struct snap_dma_completion *self, int status)
{
	enum vrdma_aq_cmd_sm_op_status op_status = VRDMA_CMD_SM_OP_OK;
	struct vrdma_admin_sw_qp *aq = container_of(self, struct vrdma_admin_sw_qp,
						                        poll_comp);

	if (status != IBV_WC_SUCCESS) {
		SPDK_ERRLOG("error in dma for vrdma admq state %d\n", aq->state);
		op_status = VRDMA_CMD_SM_OP_ERR;
	}
	vrdma_aq_cmd_progress(aq, op_status);
}

int vrdma_ctrl_adminq_progress(void *ctrl)
{
	struct vrdma_ctrl *vdev_ctrl = ctrl;
	struct vrdma_admin_sw_qp *aq = &vdev_ctrl->sw_qp;
	enum vrdma_aq_cmd_sm_op_status status = VRDMA_CMD_SM_OP_OK;
	int n = 0;

	if (!vdev_ctrl->sctrl->adminq_dma_q) {
		return 0;
	}

	if (aq->state == VRDMA_CMD_STATE_WAITING) {
		SPDK_NOTICELOG("vrdma adminq is in waiting state, cmd idx %d\n",
						aq->num_parsed);
		vrdma_aq_cmd_progress(aq, status);
		return 0;
	}

	n = snap_dma_q_progress(vdev_ctrl->sctrl->adminq_dma_q);

	if (aq->pre_ci == VRDMA_INVALID_CI_PI ||
		aq->state < VRDMA_CMD_STATE_INIT_CI) {
		return 0;
	}

	if (aq->state == VRDMA_CMD_STATE_INIT_CI) {
		vrdma_aq_sm_read_pi(aq, status);
	}

	return n;
}


void spdk_vrdma_adminq_resource_destory(struct vrdma_ctrl *ctrl)
{
	struct spdk_vrdma_dev *vdev = ctrl->vdev;
	struct spdk_vrdma_pd *vpd, *vpd_tmp;
    struct spdk_vrdma_mr *vmr, *vmr_tmp;
    struct spdk_vrdma_ah *vah, *vah_tmp;
    struct spdk_vrdma_qp *vqp, *vqp_tmp;
    struct spdk_vrdma_cq *vcq, *vcq_tmp;
    struct spdk_vrdma_eq *veq, *veq_tmp;
	struct vrdma_admin_cmd_entry aqe;

	aqe.hdr.magic = VRDMA_AQ_HDR_MEGIC_NUM;
	LIST_FOREACH_SAFE(vqp, &vdev->vqp_list, entry, vqp_tmp) {
		aqe.hdr.opcode = VRDMA_ADMIN_DESTROY_QP;
		aqe.req.destroy_qp_req.qp_handle = vqp->qp_idx;
		vrdma_aq_destroy_suspended_qp(ctrl, &aqe, vqp);
    }
    LIST_FOREACH_SAFE(vcq, &vdev->vcq_list, entry, vcq_tmp) {
		aqe.hdr.opcode = VRDMA_ADMIN_DESTROY_CQ;
		aqe.req.destroy_cq_req.cq_handle = vcq->cq_idx;
		vrdma_aq_destroy_cq(ctrl, &aqe);
    }
    LIST_FOREACH_SAFE(veq, &vdev->veq_list, entry, veq_tmp) {
		aqe.hdr.opcode = VRDMA_ADMIN_DESTROY_CEQ;
        aqe.req.destroy_ceq_req.ceq_handle = veq->eq_idx;;
        vrdma_aq_destroy_ceq(ctrl, &aqe);
    }
    LIST_FOREACH_SAFE(vah, &vdev->vah_list, entry, vah_tmp) {
		aqe.hdr.opcode = VRDMA_ADMIN_DESTROY_AH;
		aqe.req.destroy_ah_req.ah_handle = vah->ah_idx;
		vrdma_aq_destroy_ah(ctrl, &aqe);
    }
    LIST_FOREACH_SAFE(vmr, &vdev->vmr_list, entry, vmr_tmp) {
		aqe.hdr.opcode = VRDMA_ADMIN_DEREG_MR;
		aqe.req.destroy_mr_req.lkey = vmr->mr_idx;
		vrdma_aq_dereg_mr(ctrl, &aqe);
    }
    LIST_FOREACH_SAFE(vpd, &vdev->vpd_list, entry, vpd_tmp) {
		aqe.hdr.opcode = VRDMA_ADMIN_DESTROY_PD;
		aqe.req.destroy_pd_req.pd_handle = vpd->pd_idx;
		vrdma_aq_destroy_pd(ctrl, &aqe);
    }
	spdk_bit_array_free(&vdev->free_vpd_ids);
	spdk_bit_array_free(&vdev->free_vmr_ids);
	spdk_bit_array_free(&vdev->free_vah_ids);
	spdk_bit_array_free(&vdev->free_vqp_ids);
	spdk_bit_array_free(&vdev->free_vcq_ids);
	spdk_bit_array_free(&vdev->free_veq_ids);
}
