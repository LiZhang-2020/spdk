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
#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/vrdma.h"
#include "spdk/vrdma_snap.h"
#include "spdk/vrdma_emu_mgr.h"
#include "spdk/vrdma_snap_pci_mgr.h"

struct spdk_vrdma_dev_list_head spdk_vrdma_dev_list =
                              LIST_HEAD_INITIALIZER(spdk_vrdma_dev_list);

void spdk_vrdma_ctx_stop(void (*fini_cb)(void))
{
    struct spdk_vrdma_dev *vdev;

    spdk_vrdma_snap_stop(fini_cb);
    while ((vdev = LIST_FIRST(&spdk_vrdma_dev_list)) != NULL) {
        LIST_REMOVE(vdev, entry);
        spdk_emu_controller_vrdma_delete(vdev);
    }
}

int spdk_vrdma_ctx_start(struct spdk_vrdma_ctx *vrdma_ctx)
{
    struct spdk_vrdma_dev *vdev;
    struct ibv_device **list;
    int dev_count;

    if (vrdma_ctx->dpa_enabled) {
        /*Load provider just for DPA*/
    }

    if (spdk_vrdma_snap_start()) {
        SPDK_ERRLOG("Failed to start vrdma snap");
        goto err;
    }

    list = ibv_get_device_list(&dev_count);
    if (!list || !dev_count) {
        SPDK_ERRLOG("failed to open IB device list");
        goto err;
    }
    strncpy(vrdma_ctx->emu_manager, ibv_get_device_name(list[0]),
             MAX_VRDMA_DEV_LEN - 1);

    ibv_free_device_list(list);
    
    if (vrdma_ctx->dpa_enabled) {
        /*Prove init DPA*/
    }

    /*Create static PF device*/
    vdev = calloc(1, sizeof(*vdev));
    if (!vdev)
        goto err;
    vdev->emu_mgr = spdk_vrdma_snap_get_ibv_device(vrdma_ctx->emu_manager);
    vdev->devid = 0; /*lizh: Hard code for POC*/
    if (spdk_emu_controller_vrdma_create(vdev))
        goto free_vdev;

    LIST_INSERT_HEAD(&spdk_vrdma_dev_list, vdev, entry);
    return 0;
free_vdev:
    free(vdev);
err:
    return -1;
}