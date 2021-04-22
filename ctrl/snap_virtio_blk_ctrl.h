/*
 * Copyright (c) 2020 Mellanox Technologies, Inc.  All rights reserved.
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

#ifndef SNAP_VIRTIO_BLK_CTRL_H
#define SNAP_VIRTIO_BLK_CTRL_H

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "snap_virtio_common_ctrl.h"
#include "snap_virtio_blk.h"
#include "snap_blk_ops.h"
#include "snap_virtio_blk_virtq.h"

struct snap_virtio_blk_ctrl_queue {
	struct snap_virtio_ctrl_queue common;
	const struct snap_virtio_blk_queue_attr	*attr;
	struct blk_virtq_ctx *q_impl;
	bool in_error;
};

struct snap_virtio_blk_ctrl_attr {
	struct snap_virtio_ctrl_attr common;
	struct snap_virtio_blk_registers regs;
};

struct snap_virtio_blk_ctrl {
	struct snap_virtio_ctrl common;
	struct snap_bdev_ops *bdev_ops;
	void *bdev;
	uint32_t network_error;
	uint32_t internal_error;
};

struct snap_virtio_blk_ctrl *
snap_virtio_blk_ctrl_open(struct snap_context *sctx,
			  struct snap_virtio_blk_ctrl_attr *attr,
			  struct snap_bdev_ops *bdev_ops,
			  void *bdev);
void snap_virtio_blk_ctrl_close(struct snap_virtio_blk_ctrl *ctrl);
int snap_virtio_blk_ctrl_bar_setup(struct snap_virtio_blk_ctrl *ctrl,
				   struct snap_virtio_blk_registers *regs,
				   uint16_t regs_mask);
int snap_virtio_blk_ctrl_get_debugstat(struct snap_virtio_blk_ctrl *ctrl,
			struct snap_virtio_ctrl_debugstat *ctrl_debugstat);
void snap_virtio_blk_ctrl_progress(struct snap_virtio_blk_ctrl *ctrl);
void snap_virtio_blk_ctrl_io_progress(struct snap_virtio_blk_ctrl *ctrl);
void snap_virtio_blk_ctrl_io_progress_thread(struct snap_virtio_blk_ctrl *ctrl,
					     uint32_t thread_id);

#endif
