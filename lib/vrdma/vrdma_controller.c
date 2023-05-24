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

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <infiniband/verbs.h>

#include "snap.h"
#include "snap_env.h"
#include "snap_dma.h"
#include "snap_vrdma_ctrl.h"

#include "spdk/stdinc.h"
#include "spdk/bit_array.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_snap_pci_mgr.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_mr.h"
#include "spdk/vrdma_migration.h"
#include "vrdma_providers.h"
#include "spdk/vrdma_io_mgr.h"

struct vrdma_dev_mac_list_head vrdma_dev_mac_list =
				LIST_HEAD_INITIALIZER(vrdma_dev_mac_list);

void vrdma_dev_mac_add(char *pci_number, uint64_t mac, char *sf_name)
{
    struct vrdma_dev_mac *dev_mac;

    SPDK_NOTICELOG("\nConfigure vrdma device pci_number %s mac 0x%lx sf %s\n",
    pci_number, mac, sf_name);
    dev_mac = calloc(1, sizeof(*dev_mac));
    if (!dev_mac)
        return;
    memcpy(dev_mac->pci_number, pci_number, VRDMA_PCI_NAME_MAXLEN);
    memcpy(dev_mac->sf_name, sf_name, VRDMA_DEV_NAME_LEN);
    dev_mac->mac = mac;
    LIST_INSERT_HEAD(&vrdma_dev_mac_list, dev_mac, entry);
}

static struct vrdma_dev_mac *
vrdma_find_dev_mac_by_pci(char *pci_number)
{
	struct vrdma_dev_mac *dev_mac;

	LIST_FOREACH(dev_mac, &vrdma_dev_mac_list, entry) {
        SPDK_NOTICELOG("\nFind vrdma device pci_number %s: device entry pci_number %s mac 0x%lx sf %s\n",
            pci_number, dev_mac->pci_number, dev_mac->mac, dev_mac->sf_name);
        if (!strcmp(dev_mac->pci_number, pci_number))
            return dev_mac;
    }
	return NULL;
}

void vrdma_dev_mac_list_del(void)
{
    struct vrdma_dev_mac *dev_mac, *tmp;

	LIST_FOREACH_SAFE(dev_mac, &vrdma_dev_mac_list, entry, tmp) {
		LIST_REMOVE(dev_mac, entry);
		free(dev_mac);
	}
}

int vrdma_dev_name_to_id(const char *rdma_dev_name)
{
    char vrdma_dev[MAX_VRDMA_DEV_LEN];
    char *str, *next;
    int ret = 0;

    snprintf(vrdma_dev, MAX_VRDMA_DEV_LEN, "%s", rdma_dev_name);
    next = vrdma_dev;
    do {
        str = next;
        while (str[0] != '\0' && !isdigit(str[0]))
            str++;

        if (str[0] == '\0')
            break;
        else
            ret = strtol(str, &next, 0);
    } while (true);
    return ret;
}


static struct snap_context *
vrdma_ctrl_find_snap_context(const char *emu_manager, int pf_id)
{
    struct snap_context *ctx, *found = NULL;
    struct ibv_device **ibv_list;
    struct snap_pci **pf_list;
    int ibv_list_sz, pf_list_sz;
    int i, j;

    ibv_list = ibv_get_device_list(&ibv_list_sz);
    if (!ibv_list)
        return NULL;

    for (i = 0; i < ibv_list_sz; i++) {
        if (strncmp(ibv_get_device_name(ibv_list[i]), emu_manager, 16))
            continue;
        ctx = spdk_vrdma_snap_get_snap_context(ibv_get_device_name(ibv_list[i]));
        if (!ctx)
            continue;
        if (!(ctx->emulation_caps & SNAP_VRDMA))
            continue;
        pf_list = calloc(ctx->vrdma_pfs.max_pfs, sizeof(*pf_list));
        if (!pf_list)
            continue;
        pf_list_sz = snap_get_pf_list(ctx, SNAP_VRDMA, pf_list);
        for (j = 0; j < pf_list_sz; j++) {
            if (pf_list[j]->plugged && pf_list[j]->id == pf_id) {
                found = ctx;
                break;
            }
        }
        free(pf_list);
        if (found)
            break;
    }
    ibv_free_device_list(ibv_list);
    return found;
}

void vrdma_ctrl_progress(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    snap_vrdma_ctrl_progress(ctrl->sctrl);
    //spdk_vrdma_vkey_age_progress();
}

#ifndef HAVE_SPDK_POLLER_BUSY
enum spdk_thread_poller_rc { SPDK_POLLER_IDLE , SPDK_POLLER_BUSY };
#endif

static inline struct spdk_vrdma_qp *
vrmda_pg_q_entry_to_vrdma_qp(struct snap_pg_q_entry *pg_q)
{
	return container_of(pg_q, struct spdk_vrdma_qp, pg_q);
}

int vrdma_ctrl_progress_all_io(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;
	int i;

	for (i = 0; i < ctrl->sctrl->pg_ctx.npgs; i++) {
		vrdma_ctrl_progress_io(arg, i);
	}

	return SPDK_POLLER_BUSY;

    //return snap_vrdma_ctrl_io_progress(ctrl->sctrl) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

int vrdma_ctrl_progress_migration(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    vrdma_migration_progress(ctrl);
    return SPDK_POLLER_BUSY;
}

int vrdma_ctrl_progress_io(void *arg, int thread_id)
{
    struct vrdma_ctrl *ctrl = arg;
    struct snap_pg *pg = &ctrl->sctrl->pg_ctx.pgs[thread_id];
    struct spdk_vrdma_qp *vq;
	struct snap_pg_q_entry *pg_q;
    struct vrdma_tgid_node *tgid_node;

    if (is_vrdma_vqp_migration_enable() && tgid_node->src_udp[thread_id].mqp) {
        tgid_node->src_udp[thread_id].mqp->mig_ctx.mig_curr_vqp_cnt = 0;
    }
	pthread_spin_lock(&pg->lock);
  	TAILQ_FOREACH(pg_q, &pg->q_list, entry) {
		vq = vrmda_pg_q_entry_to_vrdma_qp(pg_q);
		vq->thread_id = thread_id;
		vrdma_qp_process(vq);
	}
	pthread_spin_unlock(&pg->lock);
    if (is_vrdma_vqp_migration_enable() && tgid_node->src_udp[thread_id].mqp) {
        tgid_node->src_udp[thread_id].mqp->mig_ctx.mig_curr_vqp_cnt = 0;
    }
    if (is_vrdma_vqp_migration_enable() && (!LIST_EMPTY(&vrdma_tgid_list))) {
        LIST_FOREACH(tgid_node, &vrdma_tgid_list, entry) {
            vrdma_mig_mqp_depth_sampling(tgid_node->src_udp[thread_id].mqp);
        }
    }
	return SPDK_POLLER_BUSY;

    //return snap_vrdma_ctrl_io_progress_thread(ctrl->sctrl, thread_id) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

void vrdma_ctrl_suspend(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    (void)snap_vrdma_ctrl_suspend(ctrl->sctrl);
}

bool vrdma_ctrl_is_suspended(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    if (!ctrl->sctrl)
        return true;

    return snap_vrdma_ctrl_is_suspended(ctrl->sctrl) ||
           snap_vrdma_ctrl_is_stopped(ctrl->sctrl);
}

static int vrdma_ctrl_post_flr(void *arg)
{
    struct vrdma_ctrl *ctrl = arg;

    SPDK_NOTICELOG("ctrl %p name '%s' pf_id %d : PCI FLR detected",
            ctrl, ctrl->name, ctrl->pf_id);
    /*
     * Upon FLR, we must cleanup all created mkeys, which happens
     * during spdk_ext_io_context_clear() call. As there might still
     * be IOs inflight, we should do it asynchronously from the
     * IO threads context for an orderly cleanup.
     */
    //vrdma_ctrl_io_channels_clear(ctrl, NULL, NULL);
    return 0;
}

static void vrdma_adminq_dma_cb(struct snap_dma_completion *self, int status)
{
    struct vrdma_admin_queue *admq;
    struct vrdma_admin_sw_qp *sw_qp = container_of(self,
            struct vrdma_admin_sw_qp, init_ci);

	if (status != IBV_WC_SUCCESS) {
		SPDK_ERRLOG("error in dma for init ci status %d\n", status);
	}
    admq = sw_qp->admq;
    sw_qp->pre_ci = admq->ci;
    /* pre_pi should be init as last ci*/
    sw_qp->pre_pi = sw_qp->pre_ci;
	sw_qp->state = VRDMA_CMD_STATE_INIT_CI;
}

static int vrdma_adminq_init(struct vrdma_ctrl *ctrl)
{
    struct vrdma_admin_queue *admq;
    uint32_t aq_size = sizeof(*admq);
    
    admq = spdk_malloc(aq_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!admq) {
        return -1;
    }
    ctrl->mr = ibv_reg_mr(ctrl->pd, admq, aq_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!ctrl->mr) {
        spdk_free(admq);
        return -1;
    }
    ctrl->sw_qp.pre_ci = VRDMA_INVALID_CI_PI;
    ctrl->sw_qp.pre_pi = VRDMA_INVALID_CI_PI;
    ctrl->sw_qp.poll_comp.func = vrdma_aq_sm_dma_cb;
    ctrl->sw_qp.poll_comp.count = 1;
    ctrl->sw_qp.init_ci.func = vrdma_adminq_dma_cb;
    ctrl->sw_qp.init_ci.count = 1;
    ctrl->sw_qp.admq = admq;
	ctrl->sw_qp.state = VRDMA_CMD_STATE_IDLE;
	ctrl->sw_qp.custom_sm = &vrdma_sm;
    return 0;
}

static int vrdma_device_start(void *arg)
{
	struct vrdma_prov_emu_dev_init_attr dev_init_attr = {};
	struct vrdma_prov_init_attr attr = {};
    struct vrdma_ctrl *ctrl = arg;
    int err;

    SPDK_NOTICELOG("ctrl %p name '%s' pf_id %d : vrdma_device_start\n",
            ctrl, ctrl->name, ctrl->pf_id);
	// ctx = spdk_emu_ctx_find_by_pci_id(vrdma_ctx->emu_manager, vdev->devid);
	// ctrl = ctx->ctrl;
	// ctrl->emu_ctx  = spdk_vrdma_snap_get_ibv_context(vrdma_ctx->emu_manager);
	/*now, we don't have sf, so sf_vhca_id equal emu manager vhca_id*/
	ctrl->sf_vhca_id = snap_get_dev_vhca_id(ctrl->emu_ctx);
	attr.emu_ctx = ctrl->emu_ctx;
	attr.emu_pd  = ctrl->pd;
    attr.emu_mgr_vhca_id = ctrl->sf_vhca_id;
	SPDK_NOTICELOG("start test dev for dpa\n");
	err = vrdma_prov_init(&attr, &ctrl->dpa_ctx);
	if (err) {
		SPDK_NOTICELOG("vrdma_prov_init failed\n");
		return err;
	}

	dev_init_attr.dpa_handler = ctrl->dpa_ctx;
	dev_init_attr.sf_dev_pd   = attr.emu_pd;
	dev_init_attr.sf_ibv_ctx  = attr.emu_ctx;
	dev_init_attr.sf_vhca_id  = ctrl->sf_vhca_id;
	dev_init_attr.emu_ibv_ctx = attr.emu_ctx;
	dev_init_attr.emu_vhca_id = ctrl->sctrl->sdev->pci->mpci.vhca_id;
	dev_init_attr.num_msix    = ctrl->sctrl->bar_curr->num_msix;
	dev_init_attr.msix_config_vector = ctrl->sctrl->bar_curr->msix_config;
	err = vrdma_prov_emu_dev_init(&dev_init_attr, &ctrl->dpa_emu_dev_ctx);

	if (err) {
		SPDK_NOTICELOG("vrdma_prov_emu_dev_init0 failed\n");
		goto err_prov_emu_dev_init;
	}
    return 0;

err_prov_emu_dev_init:
    vrdma_prov_uninit(ctrl->dpa_ctx);
    return err;
}

static int vrdma_device_stop(void *ctx)
{
    struct vrdma_ctrl *ctrl = ctx;
    vrdma_prov_emu_dev_uninit(ctrl->dpa_emu_dev_ctx);
    vrdma_prov_uninit(ctrl->dpa_ctx);
    return 0;
};

static bool vrdma_create_sf_pd(struct vrdma_ctrl *ctrl)
{
	struct ibv_context *dev_ctx;
	char *dev_name = ctrl->vdev->vrdma_sf.sf_name;
	int gvmi;
	
	dev_ctx = snap_vrdma_open_device(dev_name);
	if (!dev_ctx) {
		SPDK_ERRLOG("NULL dev sctx, dev name %s\n", dev_name);
		return true;
	}
	ctrl->vdev->vrdma_sf.sf_pd = ibv_alloc_pd(dev_ctx);
	if (!ctrl->vdev->vrdma_sf.sf_pd) {
		SPDK_ERRLOG("get NULL PD, dev name %s\n", dev_name);
		return true;
	}
	gvmi = snap_get_dev_vhca_id(dev_ctx);
	if (gvmi == -1) {
		SPDK_ERRLOG("get NULL gvmi, dev name %s\n", dev_name);
	} else {
		ctrl->vdev->vrdma_sf.gvmi = gvmi;
	}
	SPDK_NOTICELOG("vrdma sf dev %s(gvmi 0x%x) created pd 0x%p done\n", dev_name, gvmi,
    ctrl->vdev->vrdma_sf.sf_pd);
	return false;
}

struct vrdma_ctrl *
vrdma_ctrl_init(const struct vrdma_ctrl_init_attr *attr)
{
    struct vrdma_ctrl *ctrl;
    struct vrdma_dev_mac *dev_mac;
    struct snap_vrdma_ctrl_attr sctrl_attr = {};
    struct snap_vrdma_ctrl_bar_cbs bar_cbs = {
        .start = vrdma_device_start,
        .stop = vrdma_device_stop,
        .post_flr = vrdma_ctrl_post_flr,
    };

    ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl)
        goto err;
    ctrl->nthreads = attr->nthreads;

    ctrl->sctx = vrdma_ctrl_find_snap_context(attr->emu_manager_name,
                            attr->pf_id);
    if (!ctrl->sctx)
        goto free_ctrl;
    ctrl->pd = ibv_alloc_pd(ctrl->sctx->context);
    if (!ctrl->pd)
        goto free_ctrl;
    if (vrdma_adminq_init(ctrl))
        goto dealloc_pd;
    sctrl_attr.bar_cbs = &bar_cbs;
    sctrl_attr.cb_ctx = ctrl;
    sctrl_attr.pf_id = attr->pf_id;
    sctrl_attr.pci_type = SNAP_VRDMA_PF;
    sctrl_attr.pd = ctrl->pd;
    sctrl_attr.mr = ctrl->mr;
    sctrl_attr.npgs = attr->nthreads;
    sctrl_attr.force_in_order = attr->force_in_order;
    sctrl_attr.suspended = attr->suspended;
    sctrl_attr.adminq_dma_entry_size = VRDMA_DMA_ELEM_SIZE;
    sctrl_attr.adminq_buf = ctrl->sw_qp.admq;
    sctrl_attr.adminq_dma_comp = (struct snap_dma_completion *)&ctrl->sw_qp.init_ci;
    ctrl->sctrl = snap_vrdma_ctrl_open(ctrl->sctx, &sctrl_attr);
    if (!ctrl->sctrl) {
        SPDK_ERRLOG("Failed to open VRDMA controller %d [in order %d]"
                " over RDMA device %s, PF %d",
                attr->pf_id, attr->force_in_order, attr->emu_manager_name, attr->pf_id);
        goto dereg_mr;
    }
    ctrl->vdev = attr->vdev;
    dev_mac = vrdma_find_dev_mac_by_pci(ctrl->sctrl->sdev->pci->pci_number);
    if (dev_mac) {
        ctrl->sctrl->mac = dev_mac->mac;
        #ifdef CX7
	    ctrl->vdev->vrdma_sf.sf_pd = ibv_alloc_pd(ctrl->sctx->context);
        if(!ctrl->vdev->vrdma_sf.sf_pd) {
            SPDK_ERRLOG("Failed to create sf pd");
            goto sctrl_close;
        }
        #else
        memcpy(ctrl->vdev->vrdma_sf.sf_name, dev_mac->sf_name, VRDMA_DEV_NAME_LEN);
        if(vrdma_create_sf_pd(ctrl)) {
            SPDK_ERRLOG("Failed to create sf pd");
            goto sctrl_close;
        }
        #endif
        ctrl->crossing_mkey = snap_create_cross_mkey(ctrl->vdev->vrdma_sf.sf_pd,
								ctrl->sctrl->sdev);
        if(!ctrl->crossing_mkey) {
            SPDK_ERRLOG("Failed to create crossing mkey with sf pd");
            goto dealloc_sf_pd;
        }
    }
    snap_vrdma_device_mac_init(ctrl->sctrl);
    ctrl->pf_id = attr->pf_id;
    ctrl->dev.rdev_idx = attr->vdev->devid;
    vrdma_srv_device_init(ctrl);
    SPDK_NOTICELOG("new VRDMA controller %d [in order %d]"
                  " was opened successfully over RDMA device %s "
                  "vhca_id %d pci %s mac 0x%lx sf_name %s\n",
                  attr->pf_id, attr->force_in_order, attr->emu_manager_name,
                  ctrl->sctrl->sdev->pci->mpci.vhca_id,
                  ctrl->sctrl->sdev->pci->pci_number, ctrl->sctrl->mac,
                  ctrl->vdev->vrdma_sf.sf_name);
    snprintf(ctrl->name, VRDMA_EMU_NAME_MAXLEN,
                "%s%dpf%d", VRDMA_EMU_NAME_PREFIX,
                vrdma_dev_name_to_id(attr->emu_manager_name), attr->pf_id);
    strncpy(ctrl->emu_manager, attr->emu_manager_name,
            SPDK_EMU_MANAGER_NAME_MAXLEN - 1);
    return ctrl;
	
dealloc_sf_pd:
    ibv_dealloc_pd(ctrl->vdev->vrdma_sf.sf_pd);
sctrl_close:
    snap_vrdma_ctrl_close(ctrl->sctrl);
dereg_mr:
    ibv_dereg_mr(ctrl->mr);
    spdk_free(ctrl->sw_qp.admq);
dealloc_pd:
    ibv_dealloc_pd(ctrl->pd);
free_ctrl:
    free(ctrl);
err:
    return NULL;
}

struct vrdma_ctrl *
vrdma_find_ctrl_by_srv_dev(struct vrdma_dev *rdev)
{
    return SPDK_CONTAINEROF(rdev, struct vrdma_ctrl, dev);
}

static void vrdma_ctrl_free(struct vrdma_ctrl *ctrl)
{
    if (ctrl->mr)
        ibv_dereg_mr(ctrl->mr);
    if (ctrl->sw_qp.admq)
        spdk_free(ctrl->sw_qp.admq);
    ibv_dealloc_pd(ctrl->pd);
    if (ctrl->destroy_done_cb)
        ctrl->destroy_done_cb(ctrl->destroy_done_cb_arg);
    if (ctrl->crossing_mkey)
        snap_destroy_cross_mkey(ctrl->crossing_mkey);
    if (ctrl->vdev) {
        if (ctrl->vdev->vrdma_sf.sf_pd)
            ibv_dealloc_pd(ctrl->vdev->vrdma_sf.sf_pd);
        spdk_vrdma_adminq_resource_destory(ctrl);
        free(ctrl->vdev);
    }
    free(ctrl);
}

void vrdma_ctrl_destroy(void *arg, void (*done_cb)(void *arg),
                             void *done_cb_arg)
{
    struct vrdma_ctrl *ctrl = arg;

	if (!ctrl) {
		return;
	}
    snap_vrdma_ctrl_close(ctrl->sctrl);
	vrdma_ctrl_destroy_dma_qp(ctrl);
    ctrl->sctrl = NULL;
    ctrl->destroy_done_cb = done_cb;
    ctrl->destroy_done_cb_arg = done_cb_arg;
    vrdma_ctrl_free(ctrl);
}
