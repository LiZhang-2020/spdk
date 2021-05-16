#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_pci.h>
#include "snap_channel.h"
#include "snap_virtio_blk_virtq.h"
#include "snap_dma.h"
#include "snap_virtio_blk_ctrl.h"

#define NUM_HDR_FTR_DESCS 2

#define BDEV_SECTOR_SIZE 512
#define VIRTIO_NUM_DESC(seg_max) ((seg_max) + NUM_HDR_FTR_DESCS)
#define ERR_ON_CMD(cmd, fmt, ...) \
	snap_error("queue:%d cmd_idx:%d err: " fmt, \
		   (cmd)->vq_priv->vq_ctx.idx, (cmd)->idx, ## __VA_ARGS__)

/* uncomment to enable fast path debugging */
//#define VIRTQ_DEBUG_DATA
#ifdef VIRTQ_DEBUG_DATA
#define virtq_log_data(cmd, fmt, ...) \
	printf("queue:%d cmd_idx:%d " fmt, (cmd)->vq_priv->vq_ctx.idx, (cmd)->idx, \
	       ## __VA_ARGS__)
#else
#define virtq_log_data(cmd, fmt, ...)
#endif

#define SNAP_DMA_Q_OPMODE   "SNAP_DMA_Q_OPMODE"

struct blk_virtq_priv;

/**
 * struct split_tunnel_req_hdr - header of command received from FW
 *
 * Struct uses 2 rsvd so it will be aligned to 4B (and not 8B)
 */
struct split_tunnel_req_hdr {
	uint16_t descr_head_idx;
	uint16_t num_desc;
	uint32_t rsvd1;
	uint32_t rsvd2;
};

/**
 * struct split_tunnel_comp - header of completion sent to FW
 */
struct split_tunnel_comp {
	uint16_t descr_head_idx;
	uint16_t rsvd;
	uint32_t len;
};

/**
 * struct virtio_blk_outftr - footer of request, written to host memory
 */
struct virtio_blk_outftr {
	uint8_t status;
};

/**
 * enum virtq_cmd_sm_state - state of the sm handling a cmd
 * @VIRTQ_CMD_STATE_IDLE:               SM initialization state
 * @VIRTQ_CMD_STATE_FETCH_CMD_DESCS:    SM received tunnel cmd and copied
 *                                      immediate data, now fetch cmd descs
 * @VIRTQ_CMD_STATE_READ_REQ:           Read request data from host memory
 * @VIRTQ_CMD_STATE_HANDLE_REQ:         Handle received request from host, perform
 *                                      READ/WRITE/FLUSH
 * @VIRTQ_CMD_STATE_T_OUT_IOV_DONE:     Finished writing to bdev, check write
 *                                      status
 * @VIRTQ_CMD_STATE_T_IN_IOV_DONE:      Write data pulled from bdev to host memory
 * @VIRTQ_CMD_STATE_WRITE_STATUS:       Write cmd status to host memory
 * @VIRTQ_CMD_STATE_SEND_COMP:          Send completion to FW
 * @VIRTQ_CMD_STATE_SEND_IN_ORDER_COMP: Send completion to FW for commands completed
 *                                      unordered
 * @VIRTQ_CMD_STATE_RELEASE:            Release command
 * @VIRTQ_CMD_STATE_FATAL_ERR:          Fatal error, SM stuck here (until reset)
 */
enum virtq_cmd_sm_state {
	VIRTQ_CMD_STATE_IDLE,
	VIRTQ_CMD_STATE_FETCH_CMD_DESCS,
	VIRTQ_CMD_STATE_READ_REQ,
	VIRTQ_CMD_STATE_HANDLE_REQ,
	VIRTQ_CMD_STATE_T_OUT_IOV_DONE,
	VIRTQ_CMD_STATE_T_IN_IOV_DONE,
	VIRTQ_CMD_STATE_WRITE_STATUS,
	VIRTQ_CMD_STATE_SEND_COMP,
	VIRTQ_CMD_STATE_SEND_IN_ORDER_COMP,
	VIRTQ_CMD_STATE_RELEASE,
	VIRTQ_CMD_STATE_FATAL_ERR,
};

/**
 * enum virtq_cmd_sm_op_status - status of last operation
 * @VIRTQ_CMD_SM_OP_OK: 	Last operation finished without a problem
 * @VIRQT_CMD_SM_OP_ERR:	Last operation failed
 *
 * State machine operates asynchronously, usually by calling a function
 * and providing a callback. Once callback is called it calls the state machine
 * progress again and provides it with the status of the function called.
 * This enum describes the status of the function called.
 */
enum virtq_cmd_sm_op_status {
	VIRTQ_CMD_SM_OP_OK,
	VIRTQ_CMD_SM_OP_ERR,
};

struct blk_virtq_cmd_aux
{
	struct virtio_blk_outhdr header;
	struct split_tunnel_comp tunnel_comp;
	struct virtio_blk_outftr blk_req_ftr;
};
/**
 * struct blk_virtq_cmd - command context
 * @idx:			descr_head_idx modulo queue size
 * @descr_head_idx:	 	descriptor head index
 * @num_desc:		 	number of descriptors in the command
 * @vq_priv:		 	virtqueue command belongs to, private context
 * @state:		 	state of sm processing the command
 * @descs:		 	memory holding command descriptors
 * @buf:		 	buffer holding the request data and aux data
 * @aux:		 	aux data resided in dma/mr memory
 * @mr:                  	buf mr
 * @req_buf:		 	pointer to request buffer
 * @req_mr:		 	request buffer mr
 * @req_size:		 	allocated request buffer size
 * @dma_comp:		 	struct given to snap library
 * @total_seg_len:	 	total length of the request data to be written/read
 * @total_in_len:	 	total length of data written to request buffers
 * @use_dmem:		  	command uses dynamic mem for req_buf
 * @cmd_available_index:	sequential number of the command according to arrival
 */
struct blk_virtq_cmd {
	int idx;
	uint16_t descr_head_idx;
	size_t num_desc;
	struct blk_virtq_priv *vq_priv;
	enum virtq_cmd_sm_state state;
	struct vring_desc *descs;
	uint8_t *buf;
	size_t req_size;
	struct blk_virtq_cmd_aux *aux;
	struct ibv_mr *mr;
	uint8_t *req_buf;
	struct ibv_mr *req_mr;
	struct snap_dma_completion dma_comp;
	size_t total_seg_len;
	size_t total_in_len;
	struct snap_bdev_io_done_ctx bdev_op_ctx;
	bool use_dmem;
	blk_virtq_io_cmd_stat_t *io_cmd_stat;
	uint16_t cmd_available_index;
};

/**
 * enum blk_sw_virtq_state - state of sw virtq
 * @BLK_SW_VIRTQ_RUNNING:	Queue receives and operates commands
 * @BLK_SW_VIRTQ_FLUSHING:	Queue stops recieving new commands and operates
 * 				commands already received
 * @BLK_SW_VIRTQ_SUSPENDED:	Queue doesn't receive new commands and has no
 * 				commands to operate
 *
 * This is the state of the sw virtq (as opposed to VIRTQ_BLK_Q PRM FW object)
 */
enum blk_sw_virtq_state {
	BLK_SW_VIRTQ_RUNNING,
	BLK_SW_VIRTQ_FLUSHING,
	BLK_SW_VIRTQ_SUSPENDED,
};

/**
 * struct virtq_bdev - Backend block device
 * @ctx:	Opaque bdev context given to block device functions
 * @ops:	Block device operation pointers
 */
struct virtq_bdev {
	void *ctx;
	struct snap_bdev_ops *ops;
};

struct blk_virtq_priv {
	volatile enum blk_sw_virtq_state swq_state;
	struct blk_virtq_ctx vq_ctx;
	struct virtq_bdev blk_dev;
	struct ibv_pd *pd;
	struct snap_virtio_blk_queue *snap_vbq;
	struct snap_virtio_blk_queue_attr snap_attr;
	struct snap_dma_q *dma_q;
	struct blk_virtq_cmd *cmd_arr;
	int cmd_cntr;
	int seg_max;
	int size_max;
	int pg_id;
	struct snap_virtio_blk_ctrl_queue *vbq;
	uint16_t ctrl_available_index;
	bool force_in_order;
	/* current inorder value, for which completion should be sent */
	uint16_t ctrl_used_index;
};

static inline void virtq_mark_dirty_mem(struct blk_virtq_cmd *cmd, uint64_t pa,
					uint32_t len, bool is_completion)
{
	struct snap_virtio_ctrl_queue *vq = &cmd->vq_priv->vbq->common;
	int rc;

	if (snap_likely(!vq->log_writes_to_host))
		return;

	if (is_completion) {
		/* spec 2.6 Split Virtqueues
		 * mark all of the device area as dirty, in the worst case
		 * it will cost an extra page or two. Device area size is
		 * calculated according to the spec. */
		pa = cmd->vq_priv->snap_attr.vattr.device;
		len = 6 + 8 * cmd->vq_priv->snap_attr.vattr.size;
	}
	virtq_log_data(cmd, "MARK_DIRTY_MEM: pa 0x%lx len %u\n", pa, len);
	if (!vq->ctrl->lm_channel) {
		ERR_ON_CMD(cmd, "dirty memory logging enabled but migration channel"
			   " is not present\n");
		return;
	}
	rc = snap_channel_mark_dirty_page(vq->ctrl->lm_channel, pa, len);
	if (rc)
		ERR_ON_CMD(cmd, "mark drity page failed: pa 0x%lx len %u\n", pa, len);
}

static int blk_virtq_cmd_progress(struct blk_virtq_cmd *cmd,
				  enum virtq_cmd_sm_op_status status);

static void sm_dma_cb(struct snap_dma_completion *self, int status)
{
	enum virtq_cmd_sm_op_status op_status = VIRTQ_CMD_SM_OP_OK;
	struct blk_virtq_cmd *cmd = container_of(self,
						 struct blk_virtq_cmd,
						 dma_comp);

	if (status != IBV_WC_SUCCESS) {
		snap_error("error in dma for queue %d\n",
			   cmd->vq_priv->vq_ctx.idx);
		op_status = VIRTQ_CMD_SM_OP_ERR;
	}
	blk_virtq_cmd_progress(cmd, op_status);
}

static void bdev_io_comp_cb(enum snap_bdev_op_status status, void *done_arg);

static int init_blk_virtq_cmd(struct blk_virtq_cmd *cmd, int idx,
			      uint32_t size_max, uint32_t seg_max,
			      struct blk_virtq_priv *vq_priv)
{
	const size_t req_size = size_max * seg_max;
	const size_t descs_size = VIRTIO_NUM_DESC(seg_max) * sizeof(struct vring_desc);
	const size_t buf_size = req_size + descs_size + sizeof(struct blk_virtq_cmd_aux);

	cmd->idx = idx;
	cmd->vq_priv = vq_priv;
	cmd->dma_comp.func = sm_dma_cb;
	cmd->bdev_op_ctx.user_arg = cmd;
	cmd->bdev_op_ctx.cb = bdev_io_comp_cb;
	cmd->io_cmd_stat = NULL;
	cmd->cmd_available_index = 0;

	cmd->req_size = req_size;

	cmd->buf = vq_priv->blk_dev.ops->dma_malloc(buf_size);
	if (!cmd->buf) {
		snap_error("failed to allocate memory for virtq %d\n", idx);
		return -ENOMEM;
	}

	cmd->descs = (struct vring_desc*) ((uint8_t*) cmd->buf + req_size);
	cmd->aux = (struct blk_virtq_cmd_aux*) ((uint8_t*) cmd->descs + descs_size);

	cmd->mr = ibv_reg_mr(vq_priv->pd, cmd->buf, buf_size,
					IBV_ACCESS_REMOTE_READ |
					IBV_ACCESS_REMOTE_WRITE |
					IBV_ACCESS_LOCAL_WRITE);
	if (!cmd->mr) {
		snap_error("failed to register mr for virtq %d\n", idx);
		vq_priv->blk_dev.ops->dma_free(cmd->buf);
		return -1;
	}

	return 0;
}

void free_blk_virtq_cmds(struct blk_virtq_cmd *cmd)
{
	ibv_dereg_mr(cmd->mr);
	cmd->vq_priv->blk_dev.ops->dma_free(cmd->buf);
}

/**
 * alloc_blk_virtq_cmd_arr() - allocate memory for commands received from host
 * @size_max:	VIRTIO_BLK_F_SIZE_MAX (from virtio spec)
 * @seg_max:	VIRTIO_BLK_F_SEG_MAX (from virtio spec)
 * @vq_priv:	Block virtq private context
 *
 * Memory is allocated for command metadata, descriptors and request data.
 * Request data memory should be allocated such that it can be transfered
 * via RDMA queues and written/read to block device. Descriptors memory should
 * be allocated such that it can be written to by RDMA. Instead of registering
 * another memory region for completion allocate memory for completion mem at
 * end of the request buffer.
 * Note: for easy implementation there is a direct mapping between descr_head_idx
 * and command.
 * Todo: Unify memory into one block for all commands
 *
 * Return: Array of commands structs on success, NULL on error
 */
static struct blk_virtq_cmd *
alloc_blk_virtq_cmd_arr(uint32_t size_max, uint32_t seg_max,
			struct blk_virtq_priv *vq_priv)
{
	int i, k, ret, num = vq_priv->snap_attr.vattr.size;
	struct blk_virtq_cmd *cmd_arr;

	cmd_arr = calloc(num, sizeof(struct blk_virtq_cmd));
	if (!cmd_arr) {
		snap_error("failed to allocate memory for blk_virtq commands\n");
		goto out;
	}

	for (i = 0; i < num; i++) {
		ret = init_blk_virtq_cmd(&cmd_arr[i], i, size_max, seg_max, vq_priv);
		if (ret) {
			for (k = 0; k < i; k++)
				free_blk_virtq_cmds(&cmd_arr[k]);
			goto free_mem;
		}
	}
	return cmd_arr;

free_mem:
	free(cmd_arr);
	snap_error("failed allocating commands for queue %d\n",
	        vq_priv->vq_ctx.idx);
out:
	return NULL;
}

static void free_blk_virtq_cmd_arr(struct blk_virtq_priv *vq_priv)
{
	const size_t num_cmds = vq_priv->snap_attr.vattr.size;
	size_t i;

	for (i = 0; i < num_cmds; i++)
		free_blk_virtq_cmds(&vq_priv->cmd_arr[i]);

	free(vq_priv->cmd_arr);
}

/**
 * enum virtq_fetch_desc_status - status of descriptors fetch process
 * @VIRTQ_FETCH_DESC_DONE:	All descriptors were fetched
 * @VIRTQ_FETCH_DESC_ERR:	Error while trying to fetch a descriptor
 * @VIRTQ_FETCH_DESC_READ:	An Asynchronous read for desc was called
 */
enum virtq_fetch_desc_status {
	VIRTQ_FETCH_DESC_DONE,
	VIRTQ_FETCH_DESC_ERR,
	VIRTQ_FETCH_DESC_READ,
};

/**
 * fetch_next_desc() - Fetches command descriptors from host memory
 * @cmd: command descriptors belongs to
 *
 * Function checks if there are descriptors that were not sent in the
 * tunnled command, and if so it reads them from host memory one by one.
 * Reading from host memory is done asynchronous
 *
 * Return: virtq_fetch_desc_status
 */
static enum virtq_fetch_desc_status fetch_next_desc(struct blk_virtq_cmd *cmd)
{
	uint64_t srcaddr;
	uint16_t in_ring_desc_addr;
	int ret;

	if (cmd->num_desc == 0)
		in_ring_desc_addr = cmd->descr_head_idx %
				    cmd->vq_priv->snap_attr.vattr.size;
	else if (cmd->descs[cmd->num_desc - 1].flags & VRING_DESC_F_NEXT)
		in_ring_desc_addr = cmd->descs[cmd->num_desc - 1].next;
	else
		return VIRTQ_FETCH_DESC_DONE;

	srcaddr = cmd->vq_priv->snap_attr.vattr.desc +
		  in_ring_desc_addr * sizeof(struct vring_desc);
	cmd->dma_comp.count = 1;
	virtq_log_data(cmd, "READ_DESC: pa 0x%lx len %lu\n", srcaddr, sizeof(struct vring_desc));
	ret = snap_dma_q_read(cmd->vq_priv->dma_q, &cmd->descs[cmd->num_desc],
			      sizeof(struct vring_desc), cmd->mr->lkey,
			      srcaddr, cmd->vq_priv->snap_attr.vattr.dma_mkey,
			      &(cmd->dma_comp));
	if (ret)
		return VIRTQ_FETCH_DESC_ERR;
	cmd->num_desc++;
	return VIRTQ_FETCH_DESC_READ;
}

static void bdev_io_comp_cb(enum snap_bdev_op_status status, void *done_arg)
{
	struct blk_virtq_cmd *cmd = done_arg;
	enum virtq_cmd_sm_op_status op_status = VIRTQ_CMD_SM_OP_OK;

	if (snap_unlikely(status != SNAP_BDEV_OP_SUCCESS)) {
		snap_error("Failed iov completion!\n");
		op_status = VIRTQ_CMD_SM_OP_ERR;
		cmd->io_cmd_stat->fail++;
	}
	else
		cmd->io_cmd_stat->success++;

	blk_virtq_cmd_progress(cmd, op_status);
}

/**
 * sm_fetch_cmd_descs() - Fetch all of commands descs
 * @cmd: Command being processed
 * @status: Callback status
 *
 * Function collects all of the commands descriptors. Descriptors can be either
 * in the tunnel command itself, or in host memory.
 *
 * Return: True if state machine is moved to a new state synchronously (error
 * or all descs were fetched), false if the state transition will be done
 * asynchronously.
 */
static bool sm_fetch_cmd_descs(struct blk_virtq_cmd *cmd,
			       enum virtq_cmd_sm_op_status status)
{
	enum virtq_fetch_desc_status ret;

	if (status != VIRTQ_CMD_SM_OP_OK) {
		ERR_ON_CMD(cmd, "failed to fetch commands descs, dumping "
			   "command without response\n");
		cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
		return true;
	}
	ret = fetch_next_desc(cmd);
	if (ret == VIRTQ_FETCH_DESC_ERR) {
		ERR_ON_CMD(cmd, "failed to RDMA READ desc from host\n");
		cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
		return true;
	} else if (ret == VIRTQ_FETCH_DESC_DONE) {
		cmd->state = VIRTQ_CMD_STATE_READ_REQ;
		return true;
	} else {
		return false;
	}
}

static int virtq_alloc_req_dbuf(struct blk_virtq_cmd *cmd, size_t len)
{
	cmd->req_buf = cmd->vq_priv->blk_dev.ops->dma_malloc(len);
	if (!cmd->req_buf) {
		snap_error("failed to dynamically allocate %lu bytes for \
			   command %d request\n", len, cmd->idx);
		goto err;
	}

	cmd->req_mr = ibv_reg_mr(cmd->vq_priv->pd, cmd->req_buf, len,
				             IBV_ACCESS_REMOTE_READ |
				             IBV_ACCESS_REMOTE_WRITE |
				             IBV_ACCESS_LOCAL_WRITE);
	if (!cmd->req_mr) {
		snap_error("failed to register mr for commmand %d\n", cmd->idx);
		goto free_buf;
	}
	cmd->use_dmem = true;
	return 0;

free_buf:
	cmd->req_mr = cmd->mr;
	free(cmd->req_buf);
err:
	cmd->req_buf = cmd->buf;
	cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;
	cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
	return -1;
}

static void virtq_rel_req_dbuf(struct blk_virtq_cmd *cmd)
{
	ibv_dereg_mr(cmd->req_mr);
	cmd->vq_priv->blk_dev.ops->dma_free(cmd->req_buf);
	cmd->req_buf = cmd->buf;
	cmd->req_mr = cmd->mr;
	cmd->use_dmem = false;
}

/**
 * virtq_read_req_from_host() - Read request from host
 * @cmd: Command being processed
 *
 * RDMA READ the command request data from host memory.
 * Error after requesting the first RDMA READ is fatal because we can't
 * cancel previous RDMA READ requests done for this command, and since
 * the failing RDMA READ will not return the completion counter will not get
 * to 0 and the callback for the previous RDMA READ requests will not return.
 *
 * Handles also cases in which request is bigger than maximum buffer, so that
 * drivers which don't support the VIRTIO_BLK_F_SIZE_MAX feature will not
 * crash
 * ToDo: add non-fatal error in case first read fails
 * Note: Last desc is always VRING_DESC_F_READ
 *
 * Return: True if state machine is moved synchronously to the new state
 * (error or no data to fetch) or false if the state transition will be
 * done asynchronously.
 */
static bool virtq_read_req_from_host(struct blk_virtq_cmd *cmd)
{
	struct blk_virtq_priv *priv = cmd->vq_priv;
	size_t offset, i;
	int ret;

	if (cmd->num_desc < NUM_HDR_FTR_DESCS)
		return true;

	cmd->dma_comp.count = 1;
	for (i = 1; i < cmd->num_desc - 1; i++) {
		cmd->total_seg_len += cmd->descs[i].len;
		if ((cmd->descs[i].flags & VRING_DESC_F_WRITE) == 0)
			cmd->dma_comp.count++;
	}

	if (snap_unlikely(cmd->total_seg_len > cmd->req_size)) {
		if (virtq_alloc_req_dbuf(cmd, cmd->total_seg_len))
			return true;
	}

	offset = 0;
	cmd->state = VIRTQ_CMD_STATE_HANDLE_REQ;

	virtq_log_data(cmd, "READ_HEADER: pa 0x%llx len %u\n",
			cmd->descs[0].addr, cmd->descs[0].len);
	ret = snap_dma_q_read(priv->dma_q, &cmd->aux->header, cmd->descs[0].len,
	        cmd->mr->lkey, cmd->descs[0].addr, priv->snap_attr.vattr.dma_mkey,
	        &cmd->dma_comp);
	if (ret) {
		cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
		return true;
	}

	for (i = 1; i < cmd->num_desc - 1; i++) {
		if (cmd->descs[i].flags & VRING_DESC_F_WRITE)
			continue;

		virtq_log_data(cmd, "READ_DATA: pa 0x%llx len %u\n",
				cmd->descs[i].addr, cmd->descs[i].len);
		ret = snap_dma_q_read(priv->dma_q, cmd->req_buf + offset,
				cmd->descs[i].len, cmd->req_mr->lkey, cmd->descs[i].addr,
				priv->snap_attr.vattr.dma_mkey, &cmd->dma_comp);
		if (ret) {
			cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
			return true;
		}
		offset += cmd->descs[i].len;
	}
	return false;
}

/**
 * virtq_handle_req() - Handle received request from host
 * @cmd: Command being processed
 * @status: Callback status
 *
 * Perform commands operation (READ/WRITE/FLUSH) on backend block device.
 *
 * Return: True if state machine is moved synchronously to the new state
 * (error cases) or false if the state transition will be done asynchronously.
 */
static bool virtq_handle_req(struct blk_virtq_cmd *cmd,
			     enum virtq_cmd_sm_op_status status)
{
	struct virtq_bdev *bdev = &cmd->vq_priv->blk_dev;
	int ret, len;
	uint64_t num_blocks;
	uint32_t blk_size;
	const char *dev_name;

	if (status != VIRTQ_CMD_SM_OP_OK) {
		ERR_ON_CMD(cmd, "failed to get request data, returning"
			   " failure\n");
		cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;
		cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
		return true;
	}

	cmd->io_cmd_stat = NULL;
	switch (cmd->aux->header.type) {
	case VIRTIO_BLK_T_OUT:
		cmd->io_cmd_stat = &cmd->vq_priv->vq_ctx.io_stat.write;
		cmd->state = VIRTQ_CMD_STATE_T_OUT_IOV_DONE;
		ret = bdev->ops->write(bdev->ctx, cmd->req_buf,
				       cmd->aux->header.sector * BDEV_SECTOR_SIZE,
				       cmd->total_seg_len,
				       &cmd->bdev_op_ctx, cmd->vq_priv->pg_id);
		break;
	case VIRTIO_BLK_T_IN:
		cmd->io_cmd_stat = &cmd->vq_priv->vq_ctx.io_stat.read;
		cmd->state = VIRTQ_CMD_STATE_T_IN_IOV_DONE;
		ret = bdev->ops->read(bdev->ctx, cmd->req_buf,
				      cmd->aux->header.sector * BDEV_SECTOR_SIZE,
				      cmd->total_seg_len,
				      &cmd->bdev_op_ctx, cmd->vq_priv->pg_id);
		break;
	case VIRTIO_BLK_T_FLUSH:
		cmd->io_cmd_stat = &cmd->vq_priv->vq_ctx.io_stat.flush;
		if (cmd->aux->header.sector != 0) {
			ERR_ON_CMD(cmd, "sector must be zero for flush "
				   "command\n");
			ret = -1;
		} else {
			cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
			num_blocks = bdev->ops->get_num_blocks(bdev->ctx);
			blk_size = bdev->ops->get_block_size(bdev->ctx);
			ret = bdev->ops->flush(bdev->ctx, 0,
					       num_blocks * blk_size,
					       &cmd->bdev_op_ctx, cmd->vq_priv->pg_id);
		}
		break;
	case VIRTIO_BLK_T_GET_ID:
		cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
		dev_name = bdev->ops->get_bdev_name(bdev->ctx);
		ret = snprintf((char *)cmd->buf, cmd->req_size, "%s",
			       dev_name);
		if (ret < 0) {
			snap_error("failed to read block id\n");
			cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_UNSUPP;
			return true;
		}
		cmd->dma_comp.count = 1;
		len = snap_min(ret, cmd->descs[1].len);
		cmd->total_in_len += len;
		virtq_log_data(cmd, "WRITE_DEVID: pa 0x%llx len %u\n",
			       cmd->descs[1].addr, len);
		virtq_mark_dirty_mem(cmd, cmd->descs[1].addr, len, false);
		ret = snap_dma_q_write(cmd->vq_priv->dma_q,
				       cmd->buf,
				       len,
				       cmd->mr->lkey,
				       cmd->descs[1].addr,
				       cmd->vq_priv->snap_attr.vattr.dma_mkey,
				       &(cmd->dma_comp));
		break;
	default:
		ERR_ON_CMD(cmd, "invalid command - requested command type "
				"0x%x is not implemented\n", cmd->aux->header.type);
		cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
		cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_UNSUPP;
		return true;
	}

	if (cmd->io_cmd_stat) {
	    cmd->io_cmd_stat->total++;
	    if (ret)
	        cmd->io_cmd_stat->fail++;
	}

	if (ret) {
		ERR_ON_CMD(cmd, "failed while executing command %d \n",
		        cmd->aux->header.type);
		cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;
		cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
		return true;
	} else {
		return false;
	}
}

/**
 * sm_handle_in_iov_done() - write read data to host
 * @cmd: Command being processed
 * @status: Status of callback
 *
 * Return: True if state machine is moved synchronously to the new state
 * (error cases) or false if the state transition will be done asynchronously.
 */
static bool sm_handle_in_iov_done(struct blk_virtq_cmd *cmd,
				  enum virtq_cmd_sm_op_status status)
{
	int i, ret;
	size_t offset = 0;

	if (status != VIRTQ_CMD_SM_OP_OK) {
		ERR_ON_CMD(cmd, "failed to read from block device, send ioerr"
			   " to host\n");
		cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;
		cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
		return true;
	}

	cmd->dma_comp.count = cmd->num_desc - NUM_HDR_FTR_DESCS;
	cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
	for (i = 0; i < cmd->num_desc - NUM_HDR_FTR_DESCS; i++) {
		virtq_log_data(cmd, "WRITE_DATA: pa 0x%llx len %u\n",
			       cmd->descs[i + 1].addr, cmd->descs[i + 1].len);
		virtq_mark_dirty_mem(cmd, cmd->descs[i + 1].addr,
				     cmd->descs[i + 1].len, false);
		ret = snap_dma_q_write(cmd->vq_priv->dma_q,
				       cmd->req_buf + offset,
				       cmd->descs[i + 1].len,
				       cmd->req_mr->lkey,
				       cmd->descs[i + 1].addr,
				       cmd->vq_priv->snap_attr.vattr.dma_mkey,
				       &(cmd->dma_comp));
		if (ret) {
			cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;
			cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
			return true;
		}
		offset += cmd->descs[i + 1].len;
		cmd->total_in_len += cmd->descs[i + 1].len;
	}
	return false;
}

/**
 * sm_handle_out_iov_done() - check write to bdev result status
 * @cmd:	command which requested the write
 * @status:	status of write operation
 */
static void sm_handle_out_iov_done(struct blk_virtq_cmd *cmd,
				   enum virtq_cmd_sm_op_status status)
{
	cmd->state = VIRTQ_CMD_STATE_WRITE_STATUS;
	if (status != VIRTQ_CMD_SM_OP_OK)
		cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;
}

/**
 * sm_write_status() - Write command status to host memory upon finish
 * @cmd:	command which requested the write
 * @status:	callback status, expected 0 for no errors
 *
 * Return: True if state machine is moved synchronously to the new state
 * (error cases) or false if the state transition will be done asynchronously.
 */
static bool sm_write_status(struct blk_virtq_cmd *cmd,
			    enum virtq_cmd_sm_op_status status)
{
	int ret;

	if (status != VIRTQ_CMD_SM_OP_OK)
		cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_IOERR;

	cmd->state = VIRTQ_CMD_STATE_SEND_COMP;
	cmd->dma_comp.count = 1;
	virtq_log_data(cmd, "WRITE_STATUS: pa 0x%llx len %lu\n",
		       cmd->descs[cmd->num_desc - 1].addr,
		       sizeof(struct virtio_blk_outftr));
	virtq_mark_dirty_mem(cmd, cmd->descs[cmd->num_desc - 1].addr,
			     sizeof(struct virtio_blk_outftr), false);
	ret = snap_dma_q_write(cmd->vq_priv->dma_q, &cmd->aux->blk_req_ftr,
			       sizeof(struct virtio_blk_outftr),
			       cmd->mr->lkey,
			       cmd->descs[cmd->num_desc - 1].addr,
			       cmd->vq_priv->snap_attr.vattr.dma_mkey,
			       &cmd->dma_comp);
	if (ret) {
		cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
		return true;
	}
	cmd->total_in_len += sizeof(struct virtio_blk_outftr);
	return false;
}

/**
 * sm_send_completion() - send command completion to FW
 * @cmd: Command being processed
 * @status: Status of callback
 *
 * Return:
 * True if state machine is moved synchronously to the new state
 * (error cases) or false if the state transition will be done asynchronously.
 */
static int sm_send_completion(struct blk_virtq_cmd *cmd,
			       enum virtq_cmd_sm_op_status status)
{
	int ret;

	if (status != VIRTQ_CMD_SM_OP_OK) {
		snap_error("failed to write the request status field\n");

		/* TODO: if VIRTQ_CMD_STATE_FATAL_ERR could be recovered in the future,
		 * handle case when cmd with VIRTQ_CMD_STATE_FATAL_ERR handled unordered.
		 */
		cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
		return true;
	}

	/* check order of completed command, if the command unordered - wait for
	 * other completions
	 */
	if (snap_unlikely(cmd->vq_priv->force_in_order)) {
		if (snap_unlikely(cmd->cmd_available_index != cmd->vq_priv->ctrl_used_index)) {
			virtq_log_data(cmd, "UNORD_COMP: cmd_idx:%d, in_num:%d, wait for in_num:%d \n",
				cmd->idx, cmd->cmd_available_index, cmd->vq_priv->ctrl_used_index);
			cmd->state = VIRTQ_CMD_STATE_SEND_IN_ORDER_COMP;
			if (cmd->io_cmd_stat)
				++cmd->io_cmd_stat->unordered;

			return false;
		}
	}

	cmd->aux->tunnel_comp.descr_head_idx = cmd->descr_head_idx;
	cmd->aux->tunnel_comp.len = cmd->total_in_len;
	virtq_log_data(cmd, "SEND_COMP: descr_head_idx %d len %d send_size %lu\n",
		       cmd->aux->tunnel_comp.descr_head_idx,
		       cmd->aux->tunnel_comp.len,
		       sizeof(struct split_tunnel_comp));
	virtq_mark_dirty_mem(cmd, 0, 0, true);
	ret = snap_dma_q_send_completion(cmd->vq_priv->dma_q,
					 &cmd->aux->tunnel_comp,
					 sizeof(struct split_tunnel_comp));
	if (ret) {
		ERR_ON_CMD(cmd, "failed to second completion\n");
		cmd->state = VIRTQ_CMD_STATE_FATAL_ERR;
	} else {
		cmd->state = VIRTQ_CMD_STATE_RELEASE;
		++cmd->vq_priv->ctrl_used_index;
	}

	return true;
}

/**
 * blk_virtq_cmd_progress() - command state machine progress handle
 * @cmd:	commad to be processed
 * @status:	status of calling function (can be a callback)
 *
 * Return: 0 (Currently no option to fail)
 */
static int blk_virtq_cmd_progress(struct blk_virtq_cmd *cmd,
				  enum virtq_cmd_sm_op_status status)
{
	bool repeat = true;

	while (repeat) {
		repeat = false;
		snap_debug("virtq cmd sm state: %d\n", cmd->state);
		switch (cmd->state) {
		case VIRTQ_CMD_STATE_IDLE:
			snap_error("command in invalid state %d\n",
				   VIRTQ_CMD_STATE_IDLE);
			break;
		case VIRTQ_CMD_STATE_FETCH_CMD_DESCS:
			repeat = sm_fetch_cmd_descs(cmd, status);
			break;
		case VIRTQ_CMD_STATE_READ_REQ:
			repeat = virtq_read_req_from_host(cmd);
			break;
		case VIRTQ_CMD_STATE_HANDLE_REQ:
			repeat = virtq_handle_req(cmd, status);
			break;
		case VIRTQ_CMD_STATE_T_IN_IOV_DONE:
			repeat = sm_handle_in_iov_done(cmd, status);
			break;
		case VIRTQ_CMD_STATE_T_OUT_IOV_DONE:
			sm_handle_out_iov_done(cmd, status);
			repeat = true;
			break;
		case VIRTQ_CMD_STATE_WRITE_STATUS:
			repeat = sm_write_status(cmd, status);
			break;
		case VIRTQ_CMD_STATE_SEND_COMP:
		case VIRTQ_CMD_STATE_SEND_IN_ORDER_COMP:
			repeat = sm_send_completion(cmd, status);
			break;
		case VIRTQ_CMD_STATE_RELEASE:
			if (snap_unlikely(cmd->use_dmem))
				virtq_rel_req_dbuf(cmd);
			cmd->vq_priv->cmd_cntr--;
			break;
		case VIRTQ_CMD_STATE_FATAL_ERR:
			if (snap_unlikely(cmd->use_dmem))
				virtq_rel_req_dbuf(cmd);
			cmd->vq_priv->vq_ctx.fatal_err = -1;
			/*
			 * TODO: propagate fatal error to the controller.
			 * At the moment attempt to resume/state copy
			 * of such controller will have unpredictable
			 * results.
			 */
			cmd->vq_priv->cmd_cntr--;
			break;
		default:
			snap_error("reached invalid state %d\n", cmd->state);
		}
	};

	return 0;
}

/**
 * blk_virtq_rx_cb() - callback for new command received from host
 * @q:   	queue on which command was received
 * @data:	pointer to data sent for the command - should be
 * 		command header and optional descriptor list
 * @data_len:	length of data
 * @imm_data:	immediate data
 *
 * Received command is assigned to a memory slot in the command array according
 * to descr_head_idx. Function starts the state machine processing for this command
 */
static void blk_virtq_rx_cb(struct snap_dma_q *q, void *data,
			    uint32_t data_len, uint32_t imm_data)
{
	struct blk_virtq_priv *priv = (struct blk_virtq_priv *)q->uctx;
	void *descs = data + sizeof(struct split_tunnel_req_hdr);
	enum virtq_cmd_sm_op_status status = VIRTQ_CMD_SM_OP_OK;
	int cmd_idx, len;
	struct blk_virtq_cmd *cmd;
	struct split_tunnel_req_hdr *split_hdr;

	split_hdr = (struct split_tunnel_req_hdr *)data;

	cmd_idx = priv->ctrl_available_index % priv->snap_attr.vattr.size;
	cmd = &priv->cmd_arr[cmd_idx];
	cmd->num_desc = split_hdr->num_desc;
	cmd->descr_head_idx = split_hdr->descr_head_idx;
	cmd->total_seg_len = 0;
	cmd->total_in_len = 0;
	cmd->aux->blk_req_ftr.status = VIRTIO_BLK_S_OK;
	cmd->use_dmem = false;
	cmd->req_buf = cmd->buf;
	cmd->req_mr = cmd->mr;

	if (snap_unlikely(cmd->vq_priv->force_in_order))
		cmd->cmd_available_index = priv->ctrl_available_index;

	/* If new commands are not dropped there is a risk of never
	 * completing the flush */
	if (snap_unlikely(priv->swq_state == BLK_SW_VIRTQ_FLUSHING)) {
		virtq_log_data(cmd, "DROP_CMD: %ld inline descs, rxlen %d\n",
			       cmd->num_desc, data_len);
		return;
	}

	if (split_hdr->num_desc) {
		len = sizeof(struct vring_desc) * split_hdr->num_desc;
		memcpy(cmd->descs, descs, len);
	}

	priv->cmd_cntr++;
	priv->ctrl_available_index++;
	cmd->state = VIRTQ_CMD_STATE_FETCH_CMD_DESCS;
	virtq_log_data(cmd, "NEW_CMD: %lu inline descs, rxlen %u\n", cmd->num_desc,
		       data_len);
	blk_virtq_cmd_progress(cmd, status);
}

/**
 * blk_virtq_create() - Creates a new blk virtq object, along with RDMA QPs.
 * @vbq:	parent virt queue
 * @bdev_ops:	operations provided by bdev
 * @bdev:	Backend block device
 * @snap_dev:	Snap device on top virtq is created
 * @attr:	Configuration attributes
 *
 * Creates the snap queues, and RDMA queues. For RDMA queues
 * creates hw and sw qps, hw qps will be given to VIRTIO_BLK_Q.
 * Completion is sent inline, hence tx elem size is completion size
 * the rx queue size should match the number of possible descriptors
 * this in the worst case scenario is the VIRTQ size.
 *
 * Context: Calling function should attach the virtqueue to a polling group
 *
 * Return: NULL on failure, new block virtqueue context on success
 */
struct blk_virtq_ctx *blk_virtq_create(struct snap_virtio_blk_ctrl_queue *vbq,
				       struct snap_bdev_ops *bdev_ops,
				       void *bdev, struct snap_device *snap_dev,
				       struct blk_virtq_create_attr *attr)
{
	struct snap_dma_q_create_attr rdma_qp_create_attr = {};
	struct snap_virtio_blk_queue_attr qattr = {};
	struct blk_virtq_ctx *vq_ctx;
	struct blk_virtq_priv *vq_priv;
	int num_descs = VIRTIO_NUM_DESC(attr->seg_max);

	vq_priv = calloc(1, sizeof(struct blk_virtq_priv));
	if (!vq_priv)
		goto err;

	vq_ctx = &vq_priv->vq_ctx;
	vq_ctx->priv = vq_priv;
	vq_priv->blk_dev.ops = bdev_ops;
	vq_priv->blk_dev.ctx = bdev;
	vq_priv->pd = attr->pd;
	vq_ctx->idx = attr->idx;
	vq_ctx->fatal_err = 0;
	vq_priv->seg_max = attr->seg_max;
	vq_priv->size_max = attr->size_max;
	vq_priv->snap_attr.vattr.size = attr->queue_size;
	vq_priv->swq_state = BLK_SW_VIRTQ_RUNNING;
	vq_priv->vbq = vbq;

	vq_priv->cmd_arr = alloc_blk_virtq_cmd_arr(attr->size_max,
						   attr->seg_max, vq_priv);
	if (!vq_priv->cmd_arr) {
		snap_error("failed allocating cmd list for queue %d\n",
			   attr->idx);
		goto release_priv;
	}
	vq_priv->cmd_cntr = 0;
	vq_priv->ctrl_available_index = attr->hw_available_index;
	vq_priv->ctrl_used_index = vq_priv->ctrl_available_index;

	rdma_qp_create_attr.tx_qsize = attr->queue_size;
	rdma_qp_create_attr.tx_elem_size = sizeof(struct split_tunnel_comp);
	rdma_qp_create_attr.rx_qsize = attr->queue_size;
	rdma_qp_create_attr.rx_elem_size = sizeof(struct split_tunnel_req_hdr) +
					   num_descs * sizeof(struct vring_desc);
	rdma_qp_create_attr.uctx = vq_priv;
	rdma_qp_create_attr.rx_cb = blk_virtq_rx_cb;
	if (getenv(SNAP_DMA_Q_OPMODE))
		rdma_qp_create_attr.mode = atoi(getenv(SNAP_DMA_Q_OPMODE));
	vq_priv->dma_q = snap_dma_q_create(attr->pd, &rdma_qp_create_attr);
	if (!vq_priv->dma_q) {
		snap_error("failed creating rdma qp loop\n");
		goto dealloc_cmd_arr;
	}

	vq_priv->snap_attr.vattr.type = SNAP_VIRTQ_SPLIT_MODE;
	vq_priv->snap_attr.vattr.ev_mode = (attr->msix_vector == VIRTIO_MSI_NO_VECTOR) ?
					    SNAP_VIRTQ_NO_MSIX_MODE :
					    SNAP_VIRTQ_MSIX_MODE;
	vq_priv->snap_attr.vattr.virtio_version_1_0 = attr->virtio_version_1_0;
	vq_priv->snap_attr.vattr.offload_type = SNAP_VIRTQ_OFFLOAD_DESC_TUNNEL;
	vq_priv->snap_attr.vattr.idx = attr->idx;
	vq_priv->snap_attr.vattr.desc = attr->desc;
	vq_priv->snap_attr.vattr.driver = attr->driver;
	vq_priv->snap_attr.vattr.device = attr->device;
	vq_priv->snap_attr.vattr.full_emulation = true;
	vq_priv->snap_attr.vattr.max_tunnel_desc = snap_min(attr->max_tunnel_desc,
							    attr->seg_max + NUM_HDR_FTR_DESCS);
	vq_priv->snap_attr.vattr.event_qpn_or_msix = attr->msix_vector;
	vq_priv->snap_attr.vattr.pd = attr->pd;
	vq_priv->snap_attr.hw_available_index = attr->hw_available_index;
	vq_priv->snap_attr.hw_used_index = attr->hw_used_index;
	vq_priv->snap_attr.qp = snap_dma_q_get_fw_qp(vq_priv->dma_q);
	if (!vq_priv->snap_attr.qp) {
		snap_error("no fw qp exist when trying to create virtq\n");
		goto release_rdma_qp;
	}
	vq_priv->snap_vbq = snap_virtio_blk_create_queue(snap_dev,
							 &vq_priv->snap_attr);
	if (!vq_priv->snap_vbq) {
		snap_error("failed creating VIRTQ fw element\n");
		goto release_rdma_qp;
	}
	if (snap_virtio_blk_query_queue(vq_priv->snap_vbq,
					&vq_priv->snap_attr)) {
		snap_error("failed query created snap virtio blk queue\n");
		goto destroy_virtio_blk_queue;
	}
	qattr.vattr.state = SNAP_VIRTQ_STATE_RDY;
	if (snap_virtio_blk_modify_queue(vq_priv->snap_vbq,
					 SNAP_VIRTIO_BLK_QUEUE_MOD_STATE,
					 &qattr)) {
		snap_error("failed to change virtq to READY state\n");
		goto destroy_virtio_blk_queue;
	}

	vq_priv->force_in_order = attr->force_in_order;
	snap_debug("created VIRTQ %d succesfully in_order %d\n", attr->idx,
		   attr->force_in_order);
	return vq_ctx;

destroy_virtio_blk_queue:
	snap_virtio_blk_destroy_queue(vq_priv->snap_vbq);
release_rdma_qp:
	snap_dma_q_destroy(vq_priv->dma_q);
dealloc_cmd_arr:
	free_blk_virtq_cmd_arr(vq_priv);
release_priv:
	free(vq_priv);
err:
	snap_error("failed creating blk_virtq %d\n", attr->idx);
	return NULL;
}

/**
 * blk_virtq_destroy() - Destroyes blk virtq object
 * @q: queue to be destryoed
 *
 * Context: 1. Destroy should be called only when queue is in suspended state.
 * 	    2. blk_virtq_progress() should not be called during destruction.
 *
 * Return: void
 */
void blk_virtq_destroy(struct blk_virtq_ctx *q)
{
	struct blk_virtq_priv *vq_priv = q->priv;

	snap_debug("destroying queue %d\n", q->idx);

	if (vq_priv->swq_state != BLK_SW_VIRTQ_SUSPENDED && vq_priv->cmd_cntr)
		snap_warn("queue %d: destroying while not in the SUSPENDED state, "
			  " %d commands outstanding\n",
			  q->idx, vq_priv->cmd_cntr);

	if (snap_virtio_blk_destroy_queue(vq_priv->snap_vbq))
		snap_error("queue %d: error destroying blk_virtq\n", q->idx);

	snap_dma_q_destroy(vq_priv->dma_q);
	free_blk_virtq_cmd_arr(vq_priv);
	free(vq_priv);
}

int blk_virtq_get_debugstat(struct blk_virtq_ctx *q,
			    struct snap_virtio_queue_debugstat *q_debugstat)
{
	struct blk_virtq_priv *vq_priv = q->priv;
	struct snap_virtio_blk_queue_attr virtq_attr = {};
	struct snap_virtio_queue_counters_attr vqc_attr = {};
	struct vring_avail vra;
	struct vring_used vru;
	uint64_t drv_addr = vq_priv->snap_attr.vattr.driver;
	uint64_t dev_addr = vq_priv->snap_attr.vattr.device;
	int ret;

	ret = snap_virtio_get_vring_indexes_from_host(vq_priv->pd, drv_addr, dev_addr,
				                      vq_priv->snap_attr.vattr.dma_mkey,
					              &vra, &vru
						     );
	if (ret) {
		snap_error("failed to get vring indexes from host memory for queue %d\n", q->idx);
		return ret;
	}

	ret = snap_virtio_blk_query_queue(vq_priv->snap_vbq, &virtq_attr);
	if (ret) {
		snap_error("failed query queue %d debugstat\n", q->idx);
		return ret;
	}

	ret = snap_virtio_query_queue_counters(vq_priv->snap_vbq->virtq.ctrs_obj, &vqc_attr);
	if (ret) {
		snap_error("failed query virtio_q_counters %d debugstat\n", q->idx);
		return ret;
	}

	q_debugstat->qid = q->idx;
	q_debugstat->hw_available_index = virtq_attr.hw_available_index;
	q_debugstat->sw_available_index = vra.idx;
	q_debugstat->hw_used_index = virtq_attr.hw_used_index;
	q_debugstat->sw_used_index = vru.idx;
	q_debugstat->hw_received_descs = vqc_attr.received_desc;
	q_debugstat->hw_completed_descs = vqc_attr.completed_desc;

	return 0;
}

int blk_virtq_query_error_state(struct blk_virtq_ctx *q,
				struct snap_virtio_blk_queue_attr *attr)
{
	int ret;
	struct blk_virtq_priv *vq_priv = q->priv;

	ret = snap_virtio_blk_query_queue(vq_priv->snap_vbq, attr);
	if (ret) {
		snap_error("failed query queue %d (update)\n", q->idx);
		return ret;
	}

	if (attr->vattr.state == SNAP_VIRTQ_STATE_ERR &&
		attr->vattr.error_type == SNAP_VIRTQ_ERROR_TYPE_NO_ERROR)
		snap_warn("queue %d state is in error but error type is 0\n", q->idx);

	if (attr->vattr.state != SNAP_VIRTQ_STATE_ERR &&
		attr->vattr.error_type != SNAP_VIRTQ_ERROR_TYPE_NO_ERROR) {
		snap_warn("queue %d state is not in error but with error type %d\n",
					q->idx, attr->vattr.error_type);
	}

	return 0;
}

static int blk_virtq_progress_suspend(struct blk_virtq_ctx *q)
{
	struct blk_virtq_priv *priv = q->priv;
	struct snap_virtio_blk_queue_attr qattr = {};

	/* TODO: add option to ignore commands in the bdev layer */
	if (priv->cmd_cntr != 0)
		return 0;

	snap_dma_q_flush(priv->dma_q);

	qattr.vattr.state = SNAP_VIRTQ_STATE_SUSPEND;
	/* TODO: check with FLR/reset. I see modify fail where it should not */
	if (snap_virtio_blk_modify_queue(priv->snap_vbq, SNAP_VIRTIO_BLK_QUEUE_MOD_STATE,
					 &qattr)) {
		snap_error("queue %d: failed to move to the SUSPENDED state\n", q->idx);
	}
	/* at this point QP is in the error state and cannot be used anymore */
	snap_info("queue %d: moving to the SUSPENDED state\n", q->idx);
	priv->swq_state = BLK_SW_VIRTQ_SUSPENDED;
	return 0;
}

/**
 * blk_virq_progress_unordered() - Check & complete unordered commands
 * @vq_priv:	queue to progress
 */
static void blk_virq_progress_unordered(struct blk_virtq_priv *vq_priv)
{
	uint16_t cmd_idx = vq_priv->ctrl_used_index % vq_priv->snap_attr.vattr.size;
	struct blk_virtq_cmd* cmd = &vq_priv->cmd_arr[cmd_idx];

	while (cmd->state == VIRTQ_CMD_STATE_SEND_IN_ORDER_COMP &&
	       cmd->cmd_available_index == cmd->vq_priv->ctrl_used_index) {
		virtq_log_data(cmd, "PEND_COMP: ino_num:%d state:%d\n",
			       cmd->cmd_available_index, cmd->state);

		blk_virtq_cmd_progress(cmd, VIRTQ_CMD_SM_OP_OK);

		cmd_idx = vq_priv->ctrl_used_index % vq_priv->snap_attr.vattr.size;
		cmd = &vq_priv->cmd_arr[cmd_idx];
	}
}

/**
 * blk_virtq_progress() - Progress RDMA QPs,  Polls on QPs CQs
 * @q:	queue to progress
 *
 * Context: Not thread safe
 *
 * Return: error code on failure, 0 on success
 */
int blk_virtq_progress(struct blk_virtq_ctx *q)
{
	struct blk_virtq_priv *priv = q->priv;

	if (snap_unlikely(priv->swq_state == BLK_SW_VIRTQ_SUSPENDED))
		return 0;

	snap_dma_q_progress(priv->dma_q);

	if (snap_unlikely(priv->force_in_order))
		blk_virq_progress_unordered(priv);

	/*
	 * need to wait until all inflight requests
	 * are finished before moving to the suspend state
	 */
	if (snap_unlikely(priv->swq_state == BLK_SW_VIRTQ_FLUSHING))
		return blk_virtq_progress_suspend(q);

	return 0;
}

/**
 * blk_virtq_suspend() - Request moving queue to suspend state
 * @q:	queue to move to suspend state
 *
 * When suspend is requested the queue stops receiving new commands
 * and moves to FLUSHING state. Once all commands already fetched are
 * finished, the queue moves to SUSPENDED state.
 *
 * Context: Function is not thread safe with regard to blk_virtq_progress
 * and blk_virtq_is_suspended. If called from a different thread than
 * thread calling progress/is_suspended then application must take care of
 * proper locking
 *
 * Return: 0 on success, else error code
 */
int blk_virtq_suspend(struct blk_virtq_ctx *q)
{
	struct blk_virtq_priv *priv = q->priv;

	if (priv->swq_state != BLK_SW_VIRTQ_RUNNING) {
		snap_debug("queue %d: suspend was already requested\n",
			   q->idx);
		return -EBUSY;
	}

	snap_info("queue %d: SUSPENDING %d command(s) outstanding\n",
		  q->idx, priv->cmd_cntr);

	if (priv->vq_ctx.fatal_err)
		snap_warn("queue %d: fatal error. Resuming or live migration"
			  " will not be possible\n", q->idx);

	priv->swq_state = BLK_SW_VIRTQ_FLUSHING;
	return 0;
}

/**
 * blk_virtq_is_suspended() - api for checking if queue in suspended state
 * @q:		queue to check
 *
 * Context: Function is not thread safe with regard to blk_virtq_progress
 * and blk_virtq_suspend. If called from a different thread than
 * thread calling progress/suspend then application must take care of
 * proper locking
 *
 * Return: True when queue suspended, and False for not suspended
 */
bool blk_virtq_is_suspended(struct blk_virtq_ctx *q)
{
	struct blk_virtq_priv *priv = q->priv;
	return priv->swq_state == BLK_SW_VIRTQ_SUSPENDED;
}

/**
 * blk_virtq_start() - set virtq attributes used for operating
 * @q:  	queue to start
 * @attr:	attrs used to start the quue
 *
 * Function set attributes queue needs in order to operate.
 *
 * Return: void
 */
void blk_virtq_start(struct blk_virtq_ctx *q,
		     struct blk_virtq_start_attr *attr)
{
	struct blk_virtq_priv *priv = q->priv;

	priv->pg_id = attr->pg_id;
}

/**
 * blk_virtq_get_state() - get hw state of the queue
 * @q:      queue
 * @state:  queue state to fill
 *
 * The function fills hw_avail and hw_used indexes as seen by the controller.
 * Later the indexes can be used by the blk_virtq_create() to resume queue
 * operations.
 *
 * All other queue fields are already available in the emulation object.
 *
 * NOTE: caller should suspend queue's polling group when calling from different
 *       context.
 *
 * Return: 0 on success, -errno on failure.
 */
int blk_virtq_get_state(struct blk_virtq_ctx *q,
			struct snap_virtio_ctrl_queue_state *state)
{
	struct blk_virtq_priv *priv = q->priv;
	struct snap_virtio_blk_queue_attr attr = {};
	int ret;

	ret = snap_virtio_blk_query_queue(priv->snap_vbq, &attr);
	if (ret < 0) {
		snap_error("failed to query blk queue %d\n", q->idx);
		return ret;
	}

	/* Everything between ctrl_available_index and hw_available_index has
	 * not been touched by us. It means that the ordering still holds and
	 * it is safe to ask hw to replay all these descriptors when queue is
	 * created.
	 */
	state->hw_available_index = priv->ctrl_available_index;
	state->hw_used_index = attr.hw_used_index;
	return 0;
}

struct snap_dma_q *get_dma_q(struct blk_virtq_ctx *ctx)
{
	struct blk_virtq_priv *vpriv = ctx->priv;
	return vpriv->dma_q;
}

int set_dma_mkey(struct blk_virtq_ctx *ctx, uint32_t mkey)
{
	struct blk_virtq_priv *vpriv = ctx->priv;
	vpriv->snap_attr.vattr.dma_mkey = mkey;
	return 0;
}