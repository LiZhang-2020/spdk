/*
 * Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include <stdlib.h>
#include <arpa/inet.h>

#include "snap_dma.h"
#include "snap_env.h"
#include "mlx5_ifc.h"
#include "snap_internal.h"

#include "config.h"

/* memory barriers */

#define snap_compiler_fence() asm volatile(""::: "memory")

#if defined(__x86_64__)

#define snap_memory_bus_fence()        asm volatile("mfence"::: "memory")
#define snap_memory_bus_store_fence()  asm volatile("sfence" ::: "memory")
#define snap_memory_bus_load_fence()   asm volatile("lfence" ::: "memory")

#define snap_memory_cpu_fence()        snap_compiler_fence()
#define snap_memory_cpu_store_fence()  snap_compiler_fence()
#define snap_memory_cpu_load_fence()   snap_compiler_fence()

#elif defined(__aarch64__)

//#define snap_memory_bus_fence()        asm volatile("dsb sy" ::: "memory")
//#define snap_memory_bus_store_fence()  asm volatile("dsb st" ::: "memory")
//#define snap_memory_bus_load_fence()   asm volatile("dsb ld" ::: "memory")
//
/* The macro is used to serialize stores across Normal NC (or Device) and WB
 * memory, (see Arm Spec, B2.7.2).  Based on recent changes in Linux kernel:
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=22ec71615d824f4f11d38d0e55a88d8956b7e45f
 *
 * The underlying barrier code was changed to use lighter weight DMB instead
 * of DSB. The barrier used for synchronization of access between write back
 * and device mapped memory (PCIe BAR).
 *
 * According to vkleiner@nvidia.com
 * - improvements of around couple-hundreds kIOPS (more or less, depending
 *   on the workload) for 8 active BlueField cores with the following change
 * - improvement to parrallel fraction on 512B test
 */
#define snap_memory_bus_fence()        asm volatile("dmb oshsy" ::: "memory")
#define snap_memory_bus_store_fence()  asm volatile("dmb oshst" ::: "memory")
#define snap_memory_bus_load_fence()   asm volatile("dmb oshld" ::: "memory")

#define snap_memory_cpu_fence()        asm volatile("dmb ish" ::: "memory")
#define snap_memory_cpu_store_fence()  asm volatile("dmb ishst" ::: "memory")
#define snap_memory_cpu_load_fence()   asm volatile("dmb ishld" ::: "memory")

#else
# error "Unsupported architecture"
#endif

#define SNAP_DMA_Q_RX_CQE_SIZE  128
#define SNAP_DMA_Q_TX_CQE_SIZE  64
#define SNAP_MLX5_RECV_WQE_BB   16
#define SNAP_DMA_Q_TX_MOD_COUNT 16

SNAP_ENV_REG_ENV_VARIABLE(SNAP_DMA_Q_OPMODE, 0);
SNAP_ENV_REG_ENV_VARIABLE(SNAP_DMA_Q_IOV_SUPP, 0);

/* GGA specific */

#define MLX5_OPCODE_MMO       0x2F
#define MLX5_OPC_MOD_MMO_DMA  0x1

struct mlx5_dma_opaque {
	uint32_t syndrom;
	uint32_t reserved;
	uint32_t scattered_length;
	uint32_t gathered_length;
	uint8_t  reserved2[240];
};

struct mlx5_dma_wqe {
	uint32_t opcode;
	uint32_t sq_ds;
	uint32_t flags;
	uint32_t gga_ctrl1;  /* unused for dma */
	uint32_t gga_ctrl2;  /* unused for dma */
	uint32_t opaque_lkey;
	uint64_t opaque_vaddr;
	struct mlx5_wqe_data_seg gather;
	struct mlx5_wqe_data_seg scatter;
};

struct snap_roce_caps {
	bool resources_on_nvme_emulation_manager;
	bool roce_enabled;
	uint8_t roce_version;
	bool fl_when_roce_disabled;
	bool fl_when_roce_enabled;
	uint16_t r_roce_max_src_udp_port;
	uint16_t r_roce_min_src_udp_port;
};

static inline void snap_dv_post_recv(struct snap_dv_qp *dv_qp, void *addr,
				     size_t len, uint32_t lkey);
static inline void snap_dv_ring_rx_db(struct snap_dv_qp *dv_qp);
static int snap_dv_cq_init(struct ibv_cq *cq, struct snap_dv_cq *dv_cq);
static inline int do_dv_xfer_inline(struct snap_dma_q *q, void *src_buf, size_t len,
				    int op, uint64_t raddr, uint32_t rkey,
				    struct snap_dma_completion *flush_comp, int *n_bb);


static struct snap_dma_q_ops verb_ops;
static struct snap_dma_q_ops dv_ops;
static struct snap_dma_q_ops gga_ops;

static int fill_roce_caps(struct ibv_context *context,
			  struct snap_roce_caps *roce_caps)
{

	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret)
		return ret;

	roce_caps->resources_on_nvme_emulation_manager =
		 DEVX_GET(query_hca_cap_out, out,
		 capability.cmd_hca_cap.resources_on_nvme_emulation_manager);
	roce_caps->fl_when_roce_disabled = DEVX_GET(query_hca_cap_out, out,
		 capability.cmd_hca_cap.fl_rc_qp_when_roce_disabled);
	roce_caps->roce_enabled = DEVX_GET(query_hca_cap_out, out,
						capability.cmd_hca_cap.roce);
	if (!roce_caps->roce_enabled)
		goto out;

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_SET_HCA_CAP_OP_MOD_ROCE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret)
		return ret;

	roce_caps->roce_version = DEVX_GET(query_hca_cap_out, out,
					   capability.roce_cap.roce_version);
	roce_caps->fl_when_roce_enabled = DEVX_GET(query_hca_cap_out,
			out, capability.roce_cap.fl_rc_qp_when_roce_enabled);
	roce_caps->r_roce_max_src_udp_port = DEVX_GET(query_hca_cap_out,
			out, capability.roce_cap.r_roce_max_src_udp_port);
	roce_caps->r_roce_min_src_udp_port = DEVX_GET(query_hca_cap_out,
			out, capability.roce_cap.r_roce_min_src_udp_port);
out:
	snap_debug("RoCE Caps: enabled %d ver %d fl allowed %d\n",
		   roce_caps->roce_enabled, roce_caps->roce_version,
		   roce_caps->roce_enabled ? roce_caps->fl_when_roce_enabled :
		   roce_caps->fl_when_roce_disabled);
	return 0;
}

static int check_port(struct ibv_context *ctx, int port_num, bool *roce_en,
		      bool *ib_en, uint16_t *lid, enum ibv_mtu *mtu)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_nic_vport_context_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_nic_vport_context_out)] = {0};
	uint8_t devx_v;
	struct ibv_port_attr port_attr;
	int ret;

	*roce_en = false;
	*ib_en = false;

	ret = ibv_query_port(ctx, port_num, &port_attr);
	if (ret)
		return ret;

	if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
		/* we only support local IB addressing for now */
		if (port_attr.flags & IBV_QPF_GRH_REQUIRED) {
			snap_error("IB enabled and GRH addressing is required but only local addressing is supported\n");
			return -1;
		}
		*mtu = port_attr.active_mtu;
		*lid = port_attr.lid;
		*ib_en = true;
		return 0;
	}

	if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET)
		return -1;

	/* port may be ethernet but still have roce disabled */
	DEVX_SET(query_nic_vport_context_in, in, opcode,
		 MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
	ret = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out,
				      sizeof(out));
	if (ret) {
		snap_error("Failed to get VPORT context - assuming ROCE is disabled\n");
		return ret;
	}
	devx_v = DEVX_GET(query_nic_vport_context_out, out,
			  nic_vport_context.roce_en);
	if (devx_v)
		*roce_en = true;

	/* When active mtu is invalid, default to 1K MTU. */
	*mtu = port_attr.active_mtu ? port_attr.active_mtu : IBV_MTU_1024;
	return 0;
}

static void snap_destroy_qp_helper(struct snap_dma_ibv_qp *qp)
{
	if (qp->dv_qp.comps)
		free(qp->dv_qp.comps);

	if (qp->dv_qp.opaque_buf) {
		ibv_dereg_mr(qp->dv_qp.opaque_mr);
		free(qp->dv_qp.opaque_buf);
	}

	ibv_destroy_qp(qp->qp);
	ibv_destroy_cq(qp->rx_cq);
	ibv_destroy_cq(qp->tx_cq);
}

static void snap_free_rx_wqes(struct snap_dma_ibv_qp *qp)
{
	ibv_dereg_mr(qp->rx_mr);
	free(qp->rx_buf);
}

static int snap_alloc_rx_wqes(struct snap_dma_ibv_qp *qp, int rx_qsize,
		int rx_elem_size)
{
	struct ibv_pd *pd = qp->qp->pd;
	int rc;

	rc = posix_memalign((void **)&qp->rx_buf, SNAP_DMA_RX_BUF_ALIGN,
			rx_qsize * rx_elem_size);
	if (rc)
		return rc;

	qp->rx_mr = ibv_reg_mr(pd, qp->rx_buf, rx_qsize * rx_elem_size,
			IBV_ACCESS_LOCAL_WRITE);
	if (!qp->rx_mr) {
		free(qp->rx_buf);
		return -ENOMEM;
	}

	return 0;
}

static bool uar_memory_is_nc(struct snap_dv_qp *dv_qp)
{
	/*
	 * Verify that the memory is indeed NC. It relies on a fact (hack) that
	 * rdma-core is going to allocate NC uar if blue flame is disabled.
	 * This is a short term solution.
	 *
	 * The right solution is to allocate uars exlicitely with the
	 * mlx5dv_devx_alloc_uar()
	 */
	return dv_qp->qp.bf.size == 0;
}

static int snap_create_qp_helper(struct ibv_pd *pd, void *cq_context,
		struct ibv_comp_channel *comp_channel, int comp_vector,
		struct ibv_qp_init_attr *attr, struct snap_dma_ibv_qp *qp,
		int mode)
{
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = attr->cap.max_recv_wr,
		.cq_context = cq_context,
		.channel = comp_channel,
		.comp_vector = comp_vector,
		.wc_flags = IBV_WC_STANDARD_FLAGS,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_IGNORE_OVERRUN
	};
	struct mlx5dv_cq_init_attr cq_ex_attr = {
		.comp_mask = MLX5DV_CQ_INIT_ATTR_MASK_CQE_SIZE,
		.cqe_size = SNAP_DMA_Q_TX_CQE_SIZE
	};
	struct ibv_context *ibv_ctx = pd->context;
	struct mlx5dv_obj dv_obj;
	int rc;

	qp->mode = mode;

	if (mode == SNAP_DMA_Q_MODE_VERBS)
		qp->tx_cq = ibv_create_cq(ibv_ctx, attr->cap.max_send_wr, cq_context,
				comp_channel, comp_vector);
	else
		qp->tx_cq = ibv_cq_ex_to_cq(mlx5dv_create_cq(ibv_ctx, &cq_attr, &cq_ex_attr));
	if (!qp->tx_cq)
		return -EINVAL;

	if (mode == SNAP_DMA_Q_MODE_VERBS)
		qp->rx_cq = ibv_create_cq(ibv_ctx, attr->cap.max_recv_wr, cq_context,
					  comp_channel, comp_vector);
	else {
		/* Enable scatter to cqe on the receive side.
		 * NOTE: it seems that scatter is enabled by default in the
		 * rdma-core lib. We only have to make sure that cqe size is
		 * 128 bytes to use it.
		 **/
		cq_ex_attr.cqe_size = SNAP_DMA_Q_RX_CQE_SIZE;
		qp->rx_cq = ibv_cq_ex_to_cq(mlx5dv_create_cq(ibv_ctx, &cq_attr, &cq_ex_attr));
	}
	if (!qp->rx_cq)
		goto free_tx_cq;

	attr->qp_type = IBV_QPT_RC;
	attr->send_cq = qp->tx_cq;
	attr->recv_cq = qp->rx_cq;

	qp->qp = ibv_create_qp(pd, attr);
	if (!qp->qp)
		goto free_rx_cq;

	snap_debug("created qp 0x%x tx %d rx %d tx_inline %d on pd %p\n",
		   qp->qp->qp_num, attr->cap.max_send_wr,
		   attr->cap.max_recv_wr, attr->cap.max_inline_data, pd);

	if (mode == SNAP_DMA_Q_MODE_VERBS)
		return 0;

	dv_obj.qp.in = qp->qp;
	dv_obj.qp.out = &qp->dv_qp.qp;
	qp->dv_qp.pi = 0;
	qp->dv_qp.ci = 0;
	rc = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP);
	if (rc)
		goto free_rx_cq;

	rc = posix_memalign((void **)&qp->dv_qp.comps, SNAP_DMA_BUF_ALIGN,
			    qp->dv_qp.qp.sq.wqe_cnt * sizeof(struct snap_dv_dma_completion));
	if (rc)
		goto free_rx_cq;

	memset(qp->dv_qp.comps, 0,
	       qp->dv_qp.qp.sq.wqe_cnt * sizeof(struct snap_dv_dma_completion));

	rc = snap_dv_cq_init(qp->tx_cq, &qp->dv_tx_cq);
	if (rc)
		goto free_comps;

	rc = snap_dv_cq_init(qp->rx_cq, &qp->dv_rx_cq);
	if (rc)
		goto free_comps;

	snap_debug("sq wqe_count = %d stride = %d, rq wqe_count = %d, stride = %d, bf.reg = %p, bf.size = %d\n",
		   qp->dv_qp.qp.sq.wqe_cnt, qp->dv_qp.qp.sq.stride,
		   qp->dv_qp.qp.rq.wqe_cnt, qp->dv_qp.qp.rq.stride,
		   qp->dv_qp.qp.bf.reg, qp->dv_qp.qp.bf.size);

	qp->dv_qp.tx_db_nc = uar_memory_is_nc(&qp->dv_qp);
	if (!qp->dv_qp.tx_db_nc) {
#if defined(__aarch64__)
		snap_error("DB record must be in the non-cacheable memory on BF\n");
		goto free_comps;
#else
		snap_warn("DB record is not in the non-cacheable memory. Performance may be reduced\n"
			  "Try setting MLX5_SHUT_UP_BF environment variable\n");
#endif
	}

	if (qp->dv_qp.qp.sq.stride != MLX5_SEND_WQE_BB ||
	    qp->dv_qp.qp.rq.stride != SNAP_MLX5_RECV_WQE_BB)
		goto free_comps;

	if (mode == SNAP_DMA_Q_MODE_DV)
		return 0;

	rc = posix_memalign((void **)&qp->dv_qp.opaque_buf,
			    sizeof(struct mlx5_dma_opaque),
			    qp->dv_qp.qp.sq.wqe_cnt * sizeof(struct mlx5_dma_opaque));
	if (rc)
		goto free_comps;

	qp->dv_qp.opaque_mr = ibv_reg_mr(pd, qp->dv_qp.opaque_buf,
					 qp->dv_qp.qp.sq.wqe_cnt * sizeof(struct mlx5_dma_opaque),
					 IBV_ACCESS_LOCAL_WRITE);
	if (!qp->dv_qp.opaque_mr)
		goto free_opaque;

	return 0;

free_opaque:
	free(qp->dv_qp.opaque_buf);

	return 0;

free_comps:
	free(qp->dv_qp.comps);
free_rx_cq:
	ibv_destroy_cq(qp->rx_cq);
free_tx_cq:
	ibv_destroy_cq(qp->tx_cq);
	return -EINVAL;
}

static void snap_destroy_sw_qp(struct snap_dma_q *q)
{
	snap_free_rx_wqes(&q->sw_qp);
	snap_destroy_qp_helper(&q->sw_qp);
}

static int snap_create_sw_qp(struct snap_dma_q *q, struct ibv_pd *pd,
		struct snap_dma_q_create_attr *attr)
{
	struct ibv_qp_init_attr init_attr = {};
	struct ibv_recv_wr rx_wr, *bad_wr;
	struct ibv_sge rx_sge;
	struct snap_compression_caps comp_caps = {0};
	int i, rc;

	switch (attr->mode) {
	case SNAP_DMA_Q_MODE_AUTOSELECT:
		rc = snap_query_compression_caps(pd->context, &comp_caps);
		if (rc)
			return rc;

		if (comp_caps.dma_mmo_supported) {
			attr->mode = SNAP_DMA_Q_MODE_GGA;
			q->ops = &gga_ops;
		} else {
			attr->mode = SNAP_DMA_Q_MODE_DV;
			q->ops = &dv_ops;
		}
		break;
	case SNAP_DMA_Q_MODE_VERBS:
		q->ops = &verb_ops;
		break;
	case SNAP_DMA_Q_MODE_DV:
		q->ops = &dv_ops;
		break;
	case SNAP_DMA_Q_MODE_GGA:
		q->ops = &gga_ops;
		break;
	default:
		snap_error("Invalid SNAP_DMA_Q_OPMODE %d\n", attr->mode);
		return -EINVAL;
	}
	snap_debug("Opening dma_q of type %d\n", attr->mode);

	/* make sure that the completion is requested at least once */
	if (attr->mode != SNAP_DMA_Q_MODE_VERBS &&
	    attr->tx_qsize <= SNAP_DMA_Q_TX_MOD_COUNT)
		q->tx_qsize = SNAP_DMA_Q_TX_MOD_COUNT + 8;
	else
		q->tx_qsize = attr->tx_qsize;

	q->tx_available = q->tx_qsize;
	q->rx_elem_size = attr->rx_elem_size;
	q->tx_elem_size = attr->tx_elem_size;

	init_attr.cap.max_send_wr = attr->tx_qsize;
	/* Need more space in rx queue in order to avoid memcpy() on rx data */
	init_attr.cap.max_recv_wr = 2 * attr->rx_qsize;
	/* we must be able to send CQEs inline */
	init_attr.cap.max_inline_data = attr->tx_elem_size;

	init_attr.cap.max_send_sge = 1;
	init_attr.cap.max_recv_sge = 1;

	rc = snap_create_qp_helper(pd, attr->comp_context, attr->comp_channel,
			attr->comp_vector, &init_attr, &q->sw_qp, attr->mode);
	if (rc)
		return rc;

	if (attr->mode == SNAP_DMA_Q_MODE_DV || attr->mode == SNAP_DMA_Q_MODE_GGA)
		q->tx_available = q->sw_qp.dv_qp.qp.sq.wqe_cnt;

	rc = snap_alloc_rx_wqes(&q->sw_qp, 2 * attr->rx_qsize, attr->rx_elem_size);
	if (rc)
		goto free_qp;

	for (i = 0; i < 2 * attr->rx_qsize; i++) {
		if (attr->mode == SNAP_DMA_Q_MODE_VERBS) {
			rx_sge.addr = (uint64_t)(q->sw_qp.rx_buf +
					i * attr->rx_elem_size);
			rx_sge.length = attr->rx_elem_size;
			rx_sge.lkey = q->sw_qp.rx_mr->lkey;

			rx_wr.wr_id = rx_sge.addr;
			rx_wr.next = NULL;
			rx_wr.sg_list = &rx_sge;
			rx_wr.num_sge = 1;

			rc = ibv_post_recv(q->sw_qp.qp, &rx_wr, &bad_wr);
			if (rc)
				goto free_rx_resources;
		} else {
			snap_dv_post_recv(&q->sw_qp.dv_qp,
					  q->sw_qp.rx_buf + i * attr->rx_elem_size,
					  attr->rx_elem_size,
					  q->sw_qp.rx_mr->lkey);
			snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
		}
	}

	return 0;

free_rx_resources:
	snap_free_rx_wqes(&q->sw_qp);
free_qp:
	snap_destroy_qp_helper(&q->sw_qp);
	return rc;
}

static void snap_destroy_fw_qp(struct snap_dma_q *q)
{
	snap_destroy_qp_helper(&q->fw_qp);
}

static int snap_create_fw_qp(struct snap_dma_q *q, struct ibv_pd *pd,
			     struct snap_dma_q_create_attr *attr)
{
	struct ibv_qp_init_attr init_attr = {};
	int rc;

	/* cannot create empty cq or a qp without one */
	init_attr.cap.max_send_wr = snap_max(attr->tx_qsize / 4,
					     SNAP_DMA_FW_QP_MIN_SEND_WR);
	init_attr.cap.max_recv_wr = 1;
	/* give one sge so that we can post which is useful for testing */
	init_attr.cap.max_send_sge = 1;

	/* the qp 'resources' are going to be replaced by the fw. We do not
	 * need use DV or GGA here
	 **/
	rc = snap_create_qp_helper(pd, NULL, NULL, 0, &init_attr, &q->fw_qp, SNAP_DMA_Q_MODE_VERBS);
	return rc;
}

static int snap_modify_lb_qp_to_init(struct ibv_qp *qp,
				     struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rst2init_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rst2init_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);
	int ret;

	DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
	DEVX_SET(rst2init_qp_in, in, qpn, qp->qp_num);
	DEVX_SET(qpc, qpc, pm_state, MLX5_QP_PM_MIGRATED);

	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);

	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);

	if (attr_mask & IBV_QP_ACCESS_FLAGS) {
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_READ)
			DEVX_SET(qpc, qpc, rre, 1);
		if (qp_attr->qp_access_flags & IBV_ACCESS_REMOTE_WRITE)
			DEVX_SET(qpc, qpc, rwe, 1);
	}

	ret = mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		snap_error("failed to modify qp to init with errno = %d\n", ret);
	return ret;
}

static int snap_modify_lb_qp_to_rtr(struct ibv_qp *qp,
				    struct ibv_qp_attr *qp_attr, int attr_mask,
				    bool force_loopback, uint16_t udp_sport)
{
	uint8_t in[DEVX_ST_SZ_BYTES(init2rtr_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(init2rtr_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);
	uint8_t mac[6];
	uint8_t gid[16];
	int ret;

	DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
	DEVX_SET(init2rtr_qp_in, in, qpn, qp->qp_num);

	/* 30 is the maximum value for Infiniband QPs*/
	DEVX_SET(qpc, qpc, log_msg_max, 30);

	/* TODO: add more attributes */
	if (attr_mask & IBV_QP_PATH_MTU)
		DEVX_SET(qpc, qpc, mtu, qp_attr->path_mtu);
	if (attr_mask & IBV_QP_DEST_QPN)
		DEVX_SET(qpc, qpc, remote_qpn, qp_attr->dest_qp_num);
	if (attr_mask & IBV_QP_RQ_PSN)
		DEVX_SET(qpc, qpc, next_rcv_psn, qp_attr->rq_psn & 0xffffff);
	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_PKEY_INDEX)
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index,
			 qp_attr->pkey_index);
	if (attr_mask & IBV_QP_PORT)
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qp_attr->port_num);
	if (attr_mask & IBV_QP_MAX_DEST_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_rra_max,
			 snap_u32log2(qp_attr->max_dest_rd_atomic));
	if (attr_mask & IBV_QP_MIN_RNR_TIMER)
		DEVX_SET(qpc, qpc, min_rnr_nak, qp_attr->min_rnr_timer);
	if (attr_mask & IBV_QP_AV) {
		if (qp_attr->ah_attr.is_global) {
			DEVX_SET(qpc, qpc, primary_address_path.tclass,
				 qp_attr->ah_attr.grh.traffic_class);
			/* set destination mac */
			memcpy(gid, qp_attr->ah_attr.grh.dgid.raw, 16);
			memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
					    gid,
					    DEVX_FLD_SZ_BYTES(qpc, primary_address_path.rgid_rip));
			mac[0] = gid[8] ^ 0x02;
			mac[1] = gid[9];
			mac[2] = gid[10];
			mac[3] = gid[13];
			mac[4] = gid[14];
			mac[5] = gid[15];
			memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rmac_47_32),
					    mac, 6);

			DEVX_SET(qpc, qpc, primary_address_path.udp_sport, udp_sport);
			DEVX_SET(qpc, qpc, primary_address_path.src_addr_index,
				 qp_attr->ah_attr.grh.sgid_index);
			if (qp_attr->ah_attr.sl & 0x7)
				DEVX_SET(qpc, qpc, primary_address_path.eth_prio,
					 qp_attr->ah_attr.sl & 0x7);
			if (qp_attr->ah_attr.grh.hop_limit > 1)
				DEVX_SET(qpc, qpc, primary_address_path.hop_limit,
					 qp_attr->ah_attr.grh.hop_limit);
			else
				DEVX_SET(qpc, qpc, primary_address_path.hop_limit, 64);

			if (force_loopback)
				DEVX_SET(qpc, qpc, primary_address_path.fl, 1);
		} else {
			DEVX_SET(qpc, qpc, primary_address_path.rlid,
				 qp_attr->ah_attr.dlid);
			DEVX_SET(qpc, qpc, primary_address_path.grh, 0);
			if (qp_attr->ah_attr.sl & 0xf)
				DEVX_SET(qpc, qpc, primary_address_path.sl,
					 qp_attr->ah_attr.sl & 0xf);
		}
	}

	ret = mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		snap_error("failed to modify qp to rtr with errno = %d\n", ret);
	return ret;
}

static int snap_modify_lb_qp_to_rts(struct ibv_qp *qp,
				    struct ibv_qp_attr *qp_attr, int attr_mask)
{
	uint8_t in[DEVX_ST_SZ_BYTES(rtr2rts_qp_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(rtr2rts_qp_out)] = {0};
	void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);
	int ret;

	DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
	DEVX_SET(rtr2rts_qp_in, in, qpn, qp->qp_num);

	if (attr_mask & IBV_QP_TIMEOUT)
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qp_attr->timeout);
	if (attr_mask & IBV_QP_RETRY_CNT)
		DEVX_SET(qpc, qpc, retry_count, qp_attr->retry_cnt);
	if (attr_mask & IBV_QP_SQ_PSN)
		DEVX_SET(qpc, qpc, next_send_psn, qp_attr->sq_psn & 0xffffff);
	if (attr_mask & IBV_QP_RNR_RETRY)
		DEVX_SET(qpc, qpc, rnr_retry, qp_attr->rnr_retry);
	if (attr_mask & IBV_QP_MAX_QP_RD_ATOMIC)
		DEVX_SET(qpc, qpc, log_sra_max,
			 snap_u32log2(qp_attr->max_rd_atomic));

	ret = mlx5dv_devx_qp_modify(qp, in, sizeof(in), out, sizeof(out));
	if (ret)
		snap_error("failed to modify qp to rts with errno = %d\n", ret);
	return ret;
}

#define SNAP_IB_GRH_FLOWLABEL_MASK            (0x000FFFFF)

static uint16_t snap_get_udp_sport(uint16_t roce_min_src_udp_port,
				   uint32_t lqpn, uint32_t rqpn)
{
	/* flow_label is a field in ipv6 header, how ipv6 flow label
	 * and udp source port are related, please refer to:
	 * https://www.spinics.net/lists/linux-rdma/msg87626.html.
	 **/
	uint32_t fl, fl_low, fl_high;
	uint64_t v = (uint64_t)lqpn * rqpn;

	/* hash function to calc fl from lqpn and rqpn.
	 * a copy of rdma_calc_flow_label() from kernel
	 **/
	v ^= v >> 20;
	v ^= v >> 40;
	fl = (uint32_t)(v & SNAP_IB_GRH_FLOWLABEL_MASK);

	/* hash function to calc udp_sport from fl.
	 * a copy of rdma_flow_label_to_udp_sport() from kernel
	 **/
	fl_low = fl & 0x03FFF;
	fl_high = fl & 0xFC000;
	fl_low ^= fl_high >> 14;

	return (uint16_t)(fl_low | roce_min_src_udp_port);
}

#if !HAVE_DECL_IBV_QUERY_GID_EX
enum ibv_gid_type {
	IBV_GID_TYPE_IB,
	IBV_GID_TYPE_ROCE_V1,
	IBV_GID_TYPE_ROCE_V2,
};

struct ibv_gid_entry {
	union ibv_gid gid;
	uint32_t gid_index;
	uint32_t port_num;
	uint32_t gid_type; /* enum ibv_gid_type */
	uint32_t ndev_ifindex;
};

static int ibv_query_gid_ex(struct ibv_context *context, uint32_t port_num,
			    uint32_t gid_index, struct ibv_gid_entry *entry,
			    uint32_t flags)
{
	snap_error("%s is not implemented\n", __func__);
	return -1;
}
#endif

static int snap_activate_loop_qp(struct snap_dma_q *q, enum ibv_mtu mtu,
				 bool ib_en, uint16_t lid,
				 bool roce_en, bool force_loopback,
				 struct ibv_gid_entry *sw_gid_entry,
				 struct ibv_gid_entry *fw_gid_entry,
				 struct snap_roce_caps *roce_caps)
{
	struct ibv_qp_attr attr;
	int rc, flags_mask;
	uint16_t udp_sport;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = SNAP_DMA_QP_PKEY_INDEX;
	attr.port_num = SNAP_DMA_QP_PORT_NUM;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
	flags_mask = IBV_QP_STATE |
		     IBV_QP_PKEY_INDEX |
		     IBV_QP_PORT |
		     IBV_QP_ACCESS_FLAGS;

	rc = snap_modify_lb_qp_to_init(q->sw_qp.qp, &attr, flags_mask);
	if (rc) {
		snap_error("failed to modify SW QP to INIT errno=%d\n", rc);
		return rc;
	}

	rc = snap_modify_lb_qp_to_init(q->fw_qp.qp, &attr, flags_mask);
	if (rc) {
		snap_error("failed to modify FW QP to INIT errno=%d\n", rc);
		return rc;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.rq_psn = SNAP_DMA_QP_RQ_PSN;
	attr.max_dest_rd_atomic = SNAP_DMA_QP_MAX_DEST_RD_ATOMIC;
	attr.min_rnr_timer = SNAP_DMA_QP_RNR_TIMER;
	attr.ah_attr.port_num = SNAP_DMA_QP_PORT_NUM;
	attr.ah_attr.grh.hop_limit = SNAP_DMA_QP_HOP_LIMIT;
	if (ib_en) {
		attr.ah_attr.is_global = 0;
		attr.ah_attr.dlid = lid;
	} else {
		attr.ah_attr.is_global = 1;
	}

	attr.dest_qp_num = q->fw_qp.qp->qp_num;
	flags_mask = IBV_QP_STATE              |
		     IBV_QP_AV                 |
		     IBV_QP_PATH_MTU           |
		     IBV_QP_DEST_QPN           |
		     IBV_QP_RQ_PSN             |
		     IBV_QP_MAX_DEST_RD_ATOMIC |
		     IBV_QP_MIN_RNR_TIMER;

	if (sw_gid_entry && sw_gid_entry->gid_type == IBV_GID_TYPE_ROCE_V2 &&
		roce_caps->roce_version & MLX5_ROCE_VERSION_2_0) {
		udp_sport = snap_get_udp_sport(roce_caps->r_roce_min_src_udp_port,
				q->sw_qp.qp->qp_num, q->fw_qp.qp->qp_num);
	} else {
		udp_sport = 0;
	}

	if (roce_en && !force_loopback)
		memcpy(attr.ah_attr.grh.dgid.raw, fw_gid_entry->gid.raw,
		       sizeof(fw_gid_entry->gid.raw));
	rc = snap_modify_lb_qp_to_rtr(q->sw_qp.qp, &attr, flags_mask,
				      force_loopback, udp_sport);
	if (rc) {
		snap_error("failed to modify SW QP to RTR errno=%d\n", rc);
		return rc;
	}

	if (fw_gid_entry && fw_gid_entry->gid_type == IBV_GID_TYPE_ROCE_V2 &&
		roce_caps->roce_version & MLX5_ROCE_VERSION_2_0) {
		udp_sport = snap_get_udp_sport(roce_caps->r_roce_min_src_udp_port,
				q->fw_qp.qp->qp_num, q->sw_qp.qp->qp_num);
	} else {
		udp_sport = 0;
	}

	if (roce_en && !force_loopback)
		memcpy(attr.ah_attr.grh.dgid.raw, sw_gid_entry->gid.raw,
		       sizeof(sw_gid_entry->gid.raw));
	attr.dest_qp_num = q->sw_qp.qp->qp_num;
	rc = snap_modify_lb_qp_to_rtr(q->fw_qp.qp, &attr, flags_mask,
				      force_loopback, udp_sport);
	if (rc) {
		snap_error("failed to modify FW QP to RTR errno=%d\n", rc);
		return rc;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = SNAP_DMA_QP_TIMEOUT;
	attr.retry_cnt = SNAP_DMA_QP_RETRY_COUNT;
	attr.sq_psn = SNAP_DMA_QP_SQ_PSN;
	attr.rnr_retry = SNAP_DMA_QP_RNR_RETRY;
	attr.max_rd_atomic = SNAP_DMA_QP_MAX_RD_ATOMIC;
	flags_mask = IBV_QP_STATE              |
		     IBV_QP_TIMEOUT            |
		     IBV_QP_RETRY_CNT          |
		     IBV_QP_RNR_RETRY          |
		     IBV_QP_SQ_PSN             |
		     IBV_QP_MAX_QP_RD_ATOMIC;

	/* once QPs were moved to RTR using devx, they must also move to RTS
	 * using devx since kernel doesn't know QPs are on RTR state
	 **/
	rc = snap_modify_lb_qp_to_rts(q->sw_qp.qp, &attr, flags_mask);
	if (rc) {
		snap_error("failed to modify SW QP to RTS errno=%d\n", rc);
		return rc;
	}

	rc = snap_modify_lb_qp_to_rts(q->fw_qp.qp, &attr, flags_mask);
	if (rc) {
		snap_error("failed to modify FW QP to RTS errno=%d\n", rc);
		return rc;
	}

	return 0;
}

static int snap_connect_loop_qp(struct snap_dma_q *q)
{
	struct ibv_gid_entry sw_gid_entry, fw_gid_entry;
	int rc;
	bool roce_en = false, ib_en = false;
	uint16_t lid = 0;
	enum ibv_mtu mtu = IBV_MTU_1024;
	bool force_loopback = false;
	struct snap_roce_caps roce_caps = {0};

	rc = check_port(q->sw_qp.qp->context, SNAP_DMA_QP_PORT_NUM, &roce_en,
			&ib_en, &lid, &mtu);
	if (rc)
		return rc;

	/* If IB is supported, can immediately advance to QP activation */
	if (ib_en)
		return snap_activate_loop_qp(q, mtu, ib_en, lid, 0, 0, NULL,
					     NULL, &roce_caps);

	rc = fill_roce_caps(q->sw_qp.qp->context, &roce_caps);
	if (rc)
		return rc;

	/* Check if force-loopback is supported based on roce caps */
	if (roce_caps.resources_on_nvme_emulation_manager &&
	    ((roce_caps.roce_enabled && roce_caps.fl_when_roce_enabled) ||
	     (!roce_caps.roce_enabled && roce_caps.fl_when_roce_disabled))) {
		force_loopback = true;
	} else if (roce_en) {
		/*
		 * If force loopback is unsupported try to acquire GIDs and
		 * open a non-fl QP
		 */
		rc = ibv_query_gid_ex(q->sw_qp.qp->context, SNAP_DMA_QP_PORT_NUM,
				   SNAP_DMA_QP_GID_INDEX, &sw_gid_entry, 0);
		if (!rc)
			rc = ibv_query_gid_ex(q->fw_qp.qp->context, SNAP_DMA_QP_PORT_NUM,
					   SNAP_DMA_QP_GID_INDEX, &fw_gid_entry, 0);
		if (rc) {
			snap_error("Failed to get gid[%d] for loop QP\n",
				   SNAP_DMA_QP_GID_INDEX);
			return rc;
		}
	} else {
		snap_error("RoCE is disabled and force-loopback option is not supported. Cannot create queue\n");
		return -ENOTSUP;
	}

	return snap_activate_loop_qp(q, mtu, ib_en, lid, roce_en,
				     force_loopback, &sw_gid_entry, &fw_gid_entry, &roce_caps);
}

static int snap_create_io_ctx(struct snap_dma_q *q, struct ibv_pd *pd,
		struct snap_dma_q_create_attr *attr)
{
	int i, ret;
	struct snap_relaxed_ordering_caps caps;
	struct mlx5_devx_mkey_attr mkey_attr = {};

	q->iov_supported = false;

	if (!attr->iov_enable)
		return 0;

	/*
	 * io_ctx only required when post UMR WQE involved, and
	 * post UMR WQE is not support on stardard verbs mode.
	 */
	if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS)
		return 0;

	ret = posix_memalign((void **)&q->io_ctx, SNAP_DMA_BUF_ALIGN,
			q->tx_available * sizeof(struct snap_dma_q_io_ctx));
	if (ret) {
		snap_error("alloc dma_q io_ctx array failed");
		return -ENOMEM;
	}

	memset(q->io_ctx, 0, q->tx_available * sizeof(struct snap_dma_q_io_ctx));

	ret = snap_query_relaxed_ordering_caps(pd->context, &caps);
	if (ret) {
		snap_error("query relaxed_ordering_caps failed, ret:%d\n", ret);
		goto free_io_ctx;
	}

	TAILQ_INIT(&q->free_io_ctx);

	mkey_attr.addr = 0;
	mkey_attr.size = 0;
	mkey_attr.log_entity_size = 0;
	mkey_attr.relaxed_ordering_write = caps.relaxed_ordering_write;
	mkey_attr.relaxed_ordering_read = caps.relaxed_ordering_read;
	mkey_attr.klm_num = 0;
	mkey_attr.klm_array = NULL;

	for (i = 0; i < q->tx_available; i++) {
		q->io_ctx[i].klm_mkey = snap_create_indirect_mkey(pd, &mkey_attr);
		if (!q->io_ctx[i].klm_mkey) {
			snap_error("create klm mkey for io_ctx[%d] failed\n", i);
			goto destroy_klm_mkeys;
		}

		q->io_ctx[i].q = q;
		TAILQ_INSERT_TAIL(&q->free_io_ctx, &q->io_ctx[i], entry);
	}

	q->iov_supported = true;

	return 0;

destroy_klm_mkeys:
	for (i--; i >= 0; i--) {
		TAILQ_REMOVE(&q->free_io_ctx, &q->io_ctx[i], entry);
		snap_destroy_indirect_mkey(q->io_ctx[i].klm_mkey);
	}
free_io_ctx:
	free(q->io_ctx);
	q->io_ctx = NULL;

	return 1;
}

static void snap_destroy_io_ctx(struct snap_dma_q *q)
{
	int i;

	if (!q->io_ctx)
		return;

	for (i = 0; i < q->tx_available; i++) {
		TAILQ_REMOVE(&q->free_io_ctx, &q->io_ctx[i], entry);
		snap_destroy_indirect_mkey(q->io_ctx[i].klm_mkey);
	}

	free(q->io_ctx);
	q->io_ctx = NULL;
	q->iov_supported = false;
}

/**
 * snap_dma_q_create() - Create DMA queue
 * @pd:    protection domain to create qps
 * @attr:  dma queue creation attributes
 *
 * Create and connect both software and fw qps
 *
 * The function creates a pair of QPs and connects them.
 * snap_dma_q_get_fw_qpnum() should be used to obtain qp number that
 * can be given to firmware emulation objects.
 *
 * Note that on Blufield1 extra steps are required:
 *  - an on behalf QP with the same number as
 *    returned by the snap_dma_q_get_fw_qpnum() must be created
 *  - a fw qp state must be copied to the on behalf qp
 *  - steering rules must be set
 *
 * All these steps must be done by the application.
 *
 * Return: dma queue or NULL on error.
 */
struct snap_dma_q *snap_dma_q_create(struct ibv_pd *pd,
		struct snap_dma_q_create_attr *attr)
{
	struct snap_dma_q *q;
	int rc;

	if (!pd)
		return NULL;

	if (!attr->rx_cb)
		return NULL;

	q = calloc(1, sizeof(*q));
	if (!q)
		return NULL;

	rc = snap_create_sw_qp(q, pd, attr);
	if (rc)
		goto free_q;

	rc = snap_create_fw_qp(q, pd, attr);
	if (rc)
		goto free_sw_qp;

	rc = snap_connect_loop_qp(q);
	if (rc)
		goto free_fw_qp;

	rc = snap_create_io_ctx(q, pd, attr);
	if (rc)
		goto free_fw_qp;

	q->uctx = attr->uctx;
	q->rx_cb = attr->rx_cb;
	return q;

free_fw_qp:
	snap_destroy_fw_qp(q);
free_sw_qp:
	snap_destroy_sw_qp(q);
free_q:
	free(q);
	return NULL;
}

/**
 * snap_dma_q_destroy() - Destroy DMA queue
 *
 * @q: dma queue
 */
void snap_dma_q_destroy(struct snap_dma_q *q)
{
	snap_destroy_io_ctx(q);
	snap_destroy_sw_qp(q);
	snap_destroy_fw_qp(q);
	free(q);
}

/**
 * snap_dma_q_progress() - Progress dma queue
 * @q: dma queue
 *
 * The function progresses both send and receive operations on the given dma
 * queue.
 *
 * Send &typedef snap_dma_comp_cb_t and receive &typedef snap_dma_rx_cb_t
 * completion callbacks may be called from within this function.
 * It is guaranteed that such callbacks are called in the execution context
 * of the progress.
 *
 * If dma queue was created with a completion channel then one can
 * use it's file descriptor to check for events instead of the
 * polling. When event is detected snap_dma_q_progress() should
 * be called to process it.
 *
 * Return: number of events (send and receive) that were processed
 */
int snap_dma_q_progress(struct snap_dma_q *q)
{
	int n;

	n = q->ops->progress_tx(q);
	n += q->ops->progress_rx(q);
	return n;
}

/**
 * snap_dma_q_arm() - Request notification
 * @q: dma queue
 *
 * The function 'arms' dma queue to report send and receive events over its
 * completion channel.
 *
 * Return:  0 or -errno on error
 */
int snap_dma_q_arm(struct snap_dma_q *q)
{
	int rc;

	rc = ibv_req_notify_cq(q->sw_qp.tx_cq, 0);
	if (rc)
		return rc;

	return ibv_req_notify_cq(q->sw_qp.rx_cq, 0);
}

static inline bool qp_can_tx(struct snap_dma_q *q, int bb_needed)
{
	/* later we can also add cq space check */
	return q->tx_available >= bb_needed;
}

/**
 * snap_dma_q_flush() - Wait for outstanding operations to complete
 * @q:   dma queue
 *
 * The function waits until all outstanding operations started with
 * mlx_dma_q_read(), mlx_dma_q_write() or mlx_dma_q_send_completion() are
 * finished. The function does not progress receive operation.
 *
 * The purpose of this function is to facilitate blocking mode dma
 * and completion operations.
 *
 * Return: number of completed operations or -errno.
 */
int snap_dma_q_flush(struct snap_dma_q *q)
{
	int n, n_out, n_bb;
	int tx_available;
	struct snap_dma_completion comp;

	n = 0;
	/* in case we have tx moderation we need at least one
	 * available to be able to send a flush command
	 */
	while (!qp_can_tx(q, 1))
		n += q->ops->progress_tx(q);

	/* only dv/gga have tx moderation at the moment, flush all outstanding
	 * ops by issueing a zero length inline rdma write
	 */
	n_out = q->sw_qp.dv_qp.n_outstanding;
	if (n_out) {
		comp.count = 2;
		do_dv_xfer_inline(q, 0, 0, MLX5_OPCODE_RDMA_WRITE, 0, 0, &comp, &n_bb);
		q->tx_available -= n_bb;
		n--;
	}

	if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS)
		tx_available = q->tx_qsize;
	else
		tx_available = q->sw_qp.dv_qp.qp.sq.wqe_cnt;

	while (q->tx_available < tx_available)
		n += q->ops->progress_tx(q);

	return n_out + n;
}

/**
 * snap_dma_q_write() - DMA write to the host memory
 * @q:            dma queue
 * @src_buf:      where to get/put data
 * @len:          data length
 * @lkey:         local memory key
 * @dstaddr:      host physical or virtual address
 * @rmkey:        host memory key that describes remote memory
 * @comp:         dma completion structure
 *
 * The function starts non blocking memory transfer to the host memory. Once
 * data transfer is completed the user defined callback may be called.
 * Operations on the same dma queue are done in order.
 *
 * Return:
 * 0
 *	operation has been successfully submitted to the queue
 *	and is now in progress
 * \-EAGAIN
 *	queue does not have enough resources, must be retried later
 * < 0
 *	some other error has occured. Return value is -errno
 */
int snap_dma_q_write(struct snap_dma_q *q, void *src_buf, size_t len,
		     uint32_t lkey, uint64_t dstaddr, uint32_t rmkey,
		     struct snap_dma_completion *comp)
{
	int rc;

	if (snap_unlikely(!qp_can_tx(q, 1)))
		return -EAGAIN;

	rc = q->ops->write(q, src_buf, len, lkey, dstaddr, rmkey, comp);
	if (snap_unlikely(rc))
		return rc;

	q->tx_available--;
	return 0;
}

/**
 * snap_dma_q_writev() - DMA write to the host memory
 * @q:            dma queue
 * @src_buf:      where to get data
 * @lkey:         local memory key
 * @iov:             A scatter gather list of buffers to be read into
 * @iov_cnt:      The number of elements in @iov
 * @rmkey:        host memory key that describes remote memory
 * @comp:         dma completion structure
 *
 * The function starts non blocking memory transfer to the host memory,
 * those memory described in a scatter gather list.
 * Once data transfer is completed the user defined callback may be called.
 * Operations on the same dma queue are done in order.
 *
 * Return:
 * 0
 *     operation has been successfully submitted to the queue
 *     and is now in progress
 * \-EAGAIN
 *     queue does not have enough resources, must be retried later
 * \--ENOTSUP
 *     queue does not support write by provide a scatter gather list of buffers
 * < 0
 *     some other error has occured. Return value is -errno
 */
int snap_dma_q_writev(struct snap_dma_q *q, void *src_buf, uint32_t lkey,
				struct iovec *iov, int iov_cnt, uint32_t rmkey,
				struct snap_dma_completion *comp)
{
	int rc, n_bb;

	rc = q->ops->writev(q, src_buf, lkey, iov, iov_cnt, rmkey, comp, &n_bb);
	if (snap_unlikely(rc))
		return rc;

	q->tx_available -= n_bb;

	return 0;
}

/**
 * snap_dma_q_write_short() - DMA write of small amount of data to the
 *                            host memory
 * @q:            dma queue
 * @src_buf:      where to get data
 * @len:          data length. It must be no greater than the
 *                &struct snap_dma_q_create_attr.tx_elem_size
 * @dstaddr:      host physical or virtual address
 * @rmkey:        host memory key that describes remote memory
 *
 * The function starts non blocking memory transfer to the host memory. The
 * function is optimized to reduce latency when sending small amount of data.
 * Operations on the same dma queue are done in order.
 *
 * Note that it is safe to use @src_buf after the function returns.
 *
 * Return:
 * 0
 *	operation has been successfully submitted to the queue
 * \-EAGAIN
 *	queue does not have enough resources, must be retried later
 * < 0
 *	some other error has occured. Return value is -errno
 */
int snap_dma_q_write_short(struct snap_dma_q *q, void *src_buf, size_t len,
			   uint64_t dstaddr, uint32_t rmkey)
{
	int rc, n_bb;

	if (snap_unlikely(len > q->tx_elem_size))
		return -EINVAL;

	rc = q->ops->write_short(q, src_buf, len, dstaddr, rmkey, &n_bb);
	if (snap_unlikely(rc))
		return rc;

	q->tx_available -= n_bb;
	return 0;
}

/**
 * snap_dma_q_read() - DMA read from the host memory
 * @q:            dma queue
 * @dst_buf:      where to get/put data
 * @len:          data length
 * @lkey:         local memory key
 * @srcaddr:      host physical or virtual address
 * @rmkey:        host memory key that describes remote memory
 * @comp:         dma completion structure
 *
 * The function starts non blocking memory transfer from the host memory. Once
 * data transfer is completed the user defined callback may be called.
 * Operations on the same dma queue are done in order.
 *
 * Return:
 * 0
 *	operation has been successfully submitted to the queue
 *	and is now in progress
 * \-EAGAIN
 *	queue does not have enough resources, must be retried later
 * < 0
 *	some other error has occured. Return value is -errno
 */
int snap_dma_q_read(struct snap_dma_q *q, void *dst_buf, size_t len,
		    uint32_t lkey, uint64_t srcaddr, uint32_t rmkey,
		    struct snap_dma_completion *comp)
{
	int rc;

	if (snap_unlikely(!qp_can_tx(q, 1)))
		return -EAGAIN;

	rc = q->ops->read(q, dst_buf, len, lkey, srcaddr, rmkey, comp);
	if (snap_unlikely(rc))
		return rc;

	q->tx_available--;
	return 0;
}

/**
 * snap_dma_q_readv() - DMA read from the host memory
 * @q:            dma queue
 * @dst_buf:      where to put data
 * @lkey:         local memory key
 * @iov:             A scatter gather list of buffers to be read into
 * @iov_cnt:      The number of elements in @iov
 * @rmkey:        host memory key that describes remote memory
 * @comp:         dma completion structure
 *
 * The function starts non blocking memory transfer from the host memory,
 * those memory described in a scatter gather list.
 * Once data transfer is completed the user defined callback may be called.
 * Operations on the same dma queue are done in order.
 *
 * Return:
 * 0
 *     operation has been successfully submitted to the queue
 *     and is now in progress
 * \-EAGAIN
 *     queue does not have enough resources, must be retried later
 * \--ENOTSUP
 *     queue does not support read by provide a scatter gather list of buffers
 * < 0
 *     some other error has occured. Return value is -errno
 */
int snap_dma_q_readv(struct snap_dma_q *q, void *dst_buf, uint32_t lkey,
				struct iovec *iov, int iov_cnt, uint32_t rmkey,
				struct snap_dma_completion *comp)
{
	int rc, n_bb;

	rc = q->ops->readv(q, dst_buf, lkey, iov, iov_cnt, rmkey, comp, &n_bb);
	if (snap_unlikely(rc))
		return rc;

	q->tx_available -= n_bb;

	return 0;
}

/**
 * snap_dma_q_send_completion() - Send completion to the host
 * @q:       dma queue to
 * @src_buf: local buffer to copy the completion data from.
 * @len:     the length of completion. E.x. 16 bytes for the NVMe. It
 *           must be no greater than the value of the
 *           &struct snap_dma_q_create_attr.tx_elem_size
 *
 * The function sends a completion notification to the host. The exact meaning of
 * the 'completion' is defined by the emulation layer. For example in case of
 * NVMe it means that completion entry is placed in the completion queue and
 * MSI-X interrupt is triggered.
 *
 * Note that it is safe to use @src_buf after the function returns.
 *
 * Return:
 * 0
 *	operation has been successfully submitted to the queue
 *	and is now in progress
 * \-EAGAIN
 *	queue does not have enough resources, must be retried later
 * < 0
 *	some other error has occured. Return value is -errno
 *
 */
int snap_dma_q_send_completion(struct snap_dma_q *q, void *src_buf, size_t len)
{
	int rc, n_bb;

	if (snap_unlikely(len > q->tx_elem_size))
		return -EINVAL;

	rc = q->ops->send_completion(q, src_buf, len, &n_bb);
	if (snap_unlikely(rc))
		return rc;

	q->tx_available -= n_bb;
	return 0;
}

/**
 * snap_dma_q_get_fw_qp() - Get FW qp
 * @q:   dma queue
 *
 * The function returns qp that can be used by the FW emulation objects
 * See snap_dma_q_create() for the detailed explanation
 *
 * Return: fw qp
 */
struct ibv_qp *snap_dma_q_get_fw_qp(struct snap_dma_q *q)
{
	return q->fw_qp.qp;
}

/* Verbs implementation */

static inline int do_verbs_dma_xfer(struct snap_dma_q *q, void *buf, size_t len,
		uint32_t lkey, uint64_t raddr, uint32_t rkey, int op, int flags,
		struct snap_dma_completion *comp)
{
	struct ibv_qp *qp = q->sw_qp.qp;
	struct ibv_send_wr rdma_wr, *bad_wr;
	struct ibv_sge sge;
	int rc;

	sge.addr = (uint64_t)buf;
	sge.length = len;
	sge.lkey = lkey;

	rdma_wr.opcode = op;
	rdma_wr.send_flags = IBV_SEND_SIGNALED | flags;
	rdma_wr.num_sge = 1;
	rdma_wr.sg_list = &sge;
	rdma_wr.wr_id = (uint64_t)comp;
	rdma_wr.wr.rdma.rkey = rkey;
	rdma_wr.wr.rdma.remote_addr = raddr;
	rdma_wr.next = NULL;

	rc = ibv_post_send(qp, &rdma_wr, &bad_wr);
	if (snap_unlikely(rc))
		snap_error("DMA queue: %p failed to post opcode 0x%x\n",
			   q, op);

	return rc;
}

static inline int verbs_dma_q_write(struct snap_dma_q *q, void *src_buf, size_t len,
				    uint32_t lkey, uint64_t dstaddr, uint32_t rmkey,
				    struct snap_dma_completion *comp)
{
	return do_verbs_dma_xfer(q, src_buf, len, lkey, dstaddr, rmkey,
			IBV_WR_RDMA_WRITE, 0, comp);
}

static inline int verbs_dma_q_writev(struct snap_dma_q *q, void *src_buf,
				uint32_t lkey, struct iovec *iov, int iov_cnt,
				uint32_t rmkey, struct snap_dma_completion *comp,
				int *n_bb)
{
	return -ENOTSUP;
}

static inline int verbs_dma_q_write_short(struct snap_dma_q *q, void *src_buf,
					  size_t len, uint64_t dstaddr,
					  uint32_t rmkey, int *n_bb)
{
	*n_bb = 1;
	if (snap_unlikely(!qp_can_tx(q, *n_bb)))
		return -EAGAIN;

	return do_verbs_dma_xfer(q, src_buf, len, 0, dstaddr, rmkey,
				 IBV_WR_RDMA_WRITE, IBV_SEND_INLINE, NULL);
}

static inline int verbs_dma_q_read(struct snap_dma_q *q, void *dst_buf, size_t len,
				   uint32_t lkey, uint64_t srcaddr, uint32_t rmkey,
				   struct snap_dma_completion *comp)
{
	return do_verbs_dma_xfer(q, dst_buf, len, lkey, srcaddr, rmkey,
			IBV_WR_RDMA_READ, 0, comp);
}

static inline int verbs_dma_q_readv(struct snap_dma_q *q, void *dst_buf,
				uint32_t lkey, struct iovec *iov, int iov_cnt,
				uint32_t rmkey, struct snap_dma_completion *comp,
				int *n_bb)
{
	return -ENOTSUP;
}

static inline int verbs_dma_q_send_completion(struct snap_dma_q *q, void *src_buf,
					      size_t len, int *n_bb)
{
	struct ibv_qp *qp = q->sw_qp.qp;
	struct ibv_send_wr send_wr, *bad_wr;
	struct ibv_sge sge;
	int rc;

	*n_bb = 1;
	if (snap_unlikely(!qp_can_tx(q, *n_bb)))
		return -EAGAIN;

	sge.addr = (uint64_t)src_buf;
	sge.length = len;

	send_wr.opcode = IBV_WR_SEND;
	send_wr.send_flags = IBV_SEND_SIGNALED|IBV_SEND_INLINE;
	send_wr.num_sge = 1;
	send_wr.sg_list = &sge;
	send_wr.next = NULL;
	send_wr.wr_id = 0;

	rc = ibv_post_send(qp, &send_wr, &bad_wr);
	if (snap_unlikely(rc))
		snap_error("DMA queue %p: failed to post send: %m\n", q);

	return rc;
}

static inline int verbs_dma_q_progress_rx(struct snap_dma_q *q)
{
	struct ibv_wc wcs[SNAP_DMA_MAX_RX_COMPLETIONS];
	int i, n;
	int rc;
	struct ibv_recv_wr rx_wr[SNAP_DMA_MAX_RX_COMPLETIONS + 1], *bad_wr;
	struct ibv_sge rx_sge[SNAP_DMA_MAX_RX_COMPLETIONS + 1];

	n = ibv_poll_cq(q->sw_qp.rx_cq, SNAP_DMA_MAX_RX_COMPLETIONS, wcs);
	if (n == 0)
		return 0;

	if (snap_unlikely(n < 0)) {
		snap_error("dma queue %p: failed to poll rx cq: errno=%d\n", q, n);
		return 0;
	}

	for (i = 0; i < n; i++) {
		if (snap_unlikely(wcs[i].status != IBV_WC_SUCCESS)) {
			if (wcs[i].status == IBV_WC_WR_FLUSH_ERR) {
				snap_debug("dma queue %p: got FLUSH_ERROR\n", q);
			} else {
				snap_error("dma queue %p: got unexpected completion status 0x%x, opcode 0x%x\n",
					   q, wcs[i].status, wcs[i].opcode);
			}
			return n;
		}

		q->rx_cb(q, (void *)wcs[i].wr_id, wcs[i].byte_len,
				wcs[i].imm_data);
		rx_sge[i].addr = wcs[i].wr_id;
		rx_sge[i].length = q->rx_elem_size;
		rx_sge[i].lkey = q->sw_qp.rx_mr->lkey;

		rx_wr[i].wr_id = rx_sge[i].addr;
		rx_wr[i].next = &rx_wr[i + 1];
		rx_wr[i].sg_list = &rx_sge[i];
		rx_wr[i].num_sge = 1;
	}

	rx_wr[i - 1].next = NULL;
	rc = ibv_post_recv(q->sw_qp.qp, rx_wr, &bad_wr);
	if (snap_unlikely(rc))
		snap_error("dma queue %p: failed to post recv: errno=%d\n",
				q, rc);
	return n;
}

static inline int verbs_dma_q_progress_tx(struct snap_dma_q *q)
{
	struct ibv_wc wcs[SNAP_DMA_MAX_TX_COMPLETIONS];
	struct snap_dma_completion *comp;
	int i, n;

	n = ibv_poll_cq(q->sw_qp.tx_cq, SNAP_DMA_MAX_TX_COMPLETIONS, wcs);
	if (n == 0)
		return 0;

	if (snap_unlikely(n < 0)) {
		snap_error("dma queue %p: failed to poll tx cq: errno=%d\n",
				q, n);
		return 0;
	}

	q->tx_available += n;

	for (i = 0; i < n; i++) {
		if (snap_unlikely(wcs[i].status != IBV_WC_SUCCESS))
			snap_error("dma queue %p: got unexpected completion status 0x%x, opcode 0x%x\n",
				   q, wcs[i].status, wcs[i].opcode);
		/* wr_id, status, qp_num and vendor_err are still valid in
		 * case of error
		 **/
		comp = (struct snap_dma_completion *)wcs[i].wr_id;
		if (!comp)
			continue;

		if (--comp->count == 0)
			comp->func(comp, wcs[i].status);
	}

	return n;
}

static struct snap_dma_q_ops verb_ops = {
	.write           = verbs_dma_q_write,
	.writev           = verbs_dma_q_writev,
	.write_short     = verbs_dma_q_write_short,
	.read            = verbs_dma_q_read,
	.readv            = verbs_dma_q_readv,
	.send_completion = verbs_dma_q_send_completion,
	.progress_tx     = verbs_dma_q_progress_tx,
	.progress_rx     = verbs_dma_q_progress_rx,
};

/* DV implementation */
static inline int snap_dv_get_cq_update(struct snap_dv_qp *dv_qp, struct snap_dma_completion *comp)
{
	if (comp || dv_qp->n_outstanding + 1 >= SNAP_DMA_Q_TX_MOD_COUNT)
		return MLX5_WQE_CTRL_CQ_UPDATE;
	else
		return 0;
}

static inline void snap_dv_set_comp(struct snap_dv_qp *dv_qp, uint16_t pi,
				    struct snap_dma_completion *comp, int fm_ce_se, int n_bb)
{
	dv_qp->comps[pi].comp = comp;
	if ((fm_ce_se & MLX5_WQE_CTRL_CQ_UPDATE) != MLX5_WQE_CTRL_CQ_UPDATE) {
		dv_qp->n_outstanding += n_bb;
		return;
	}

	dv_qp->comps[pi].n_outstanding = dv_qp->n_outstanding + n_bb;
	dv_qp->n_outstanding = 0;
}

static int snap_dv_cq_init(struct ibv_cq *cq, struct snap_dv_cq *dv_cq)
{
	struct mlx5dv_obj dv_obj;
	int rc;

	dv_cq->ci = 0;
	dv_obj.cq.in = cq;
	dv_obj.cq.out = &dv_cq->cq;
	rc = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_CQ);

	snap_debug("dv_cq: cqn = 0x%x, cqe_size = %d, cqe_count = %d comp_mask = 0x0%lx\n",
		   dv_cq->cq.cqn, dv_cq->cq.cqe_size, dv_cq->cq.cqe_cnt,
		   dv_cq->cq.comp_mask);
	return rc;
}

static inline void snap_dv_ring_tx_db(struct snap_dv_qp *dv_qp, struct mlx5_wqe_ctrl_seg *ctrl)
{
	dv_qp->pi++;
	/* 8.9.3.1  Posting a Work Request to Work Queue
	 * 1. Write WQE to the WQE buffer sequentially to previously-posted
	 * WQE (on WQEBB granularity)
	 *
	 * Use cpu barrier to prevent code reordering
	 */
	snap_memory_cpu_store_fence();

	/* 2. Update Doorbell Record associated with that queue by writing
	 *    the sq_wqebb_counter or wqe_counter for send and RQ respectively
	 **/
	dv_qp->qp.dbrec[MLX5_SND_DBR] = htobe32(dv_qp->pi);

	/* Make sure that doorbell record is written before ringing the doorbell
	 **/
	snap_memory_bus_store_fence();

	/* 3. For send request ring DoorBell by writing to the Doorbell
	 *    Register field in the UAR associated with that queue
	 */
	*(uint64_t *)(dv_qp->qp.bf.reg) = *(uint64_t *)ctrl;

	/* If UAR is mapped as WC (write combined) we need another fence to
	 * force write. Otherwise it may take a long time.
	 * On BF2/1 uar is mapped as NC (non combined) and fence is not needed
	 * here.
	 */
#if !defined(__aarch64__)
	if (!dv_qp->tx_db_nc)
		snap_memory_bus_store_fence();
#endif
}

static inline void snap_dv_ring_rx_db(struct snap_dv_qp *dv_qp)
{
	snap_memory_cpu_store_fence();
	dv_qp->qp.dbrec[MLX5_RCV_DBR] = htobe32(dv_qp->ci);
	snap_memory_bus_store_fence();
}

static inline void snap_dv_post_recv(struct snap_dv_qp *dv_qp, void *addr,
				     size_t len, uint32_t lkey)
{
	struct mlx5_wqe_data_seg *dseg;

	dseg = (struct mlx5_wqe_data_seg *)(dv_qp->qp.rq.buf + (dv_qp->ci & (dv_qp->qp.rq.wqe_cnt - 1)) *
					    SNAP_MLX5_RECV_WQE_BB);
	mlx5dv_set_data_seg(dseg, len, lkey, (intptr_t)addr);
	dv_qp->ci++;
}

static inline void *snap_dv_get_wqe_bb(struct snap_dv_qp *dv_qp)
{
	return dv_qp->qp.sq.buf + (dv_qp->pi & (dv_qp->qp.sq.wqe_cnt - 1)) *
	       MLX5_SEND_WQE_BB;
}

static inline uint16_t round_up(uint16_t x, uint16_t d)
{
	return (x + d - 1)/d;
}

static inline void snap_set_umr_inline_klm_seg(union mlx5_wqe_umr_inline_seg *klm,
					struct mlx5_klm *mtt)
{
	klm->klm.byte_count = htobe32(mtt->byte_count);
	klm->klm.mkey = htobe32(mtt->mkey);
	klm->klm.address = htobe64(mtt->address);
}

static inline void snap_set_umr_mkey_seg(struct mlx5_wqe_mkey_context_seg *mkey,
					struct mlx5_klm *klm_mtt, int klm_entries)
{
	int i;
	uint64_t len = 0;

	mkey->free = 0;
	mkey->start_addr = htobe64(klm_mtt[0].address);

	for (i = 0; i < klm_entries; i++)
		len += klm_mtt[i].byte_count;

	mkey->len = htobe64(len);
}

static inline void snap_set_umr_control_seg(struct mlx5_wqe_umr_ctrl_seg *ctrl,
					int klm_entries)
{
	/* explicitly set rsvd0 and rsvd1 from struct mlx5_wqe_umr_ctrl_seg to 0,
	 * otherwise post umr wqe will fail if reuse those WQE BB with dirty data.
	 **/
	*(uint32_t *)ctrl = 0;
	*((uint64_t *)ctrl + 2) = 0;
	*((uint64_t *)ctrl + 3) = 0;
	*((uint64_t *)ctrl + 4) = 0;
	*((uint64_t *)ctrl + 5) = 0;

	ctrl->flags = MLX5_WQE_UMR_CTRL_FLAG_INLINE |
				MLX5_WQE_UMR_CTRL_FLAG_TRNSLATION_OFFSET;

	/* if use a non-zero offset value, should use htobe16(offset) */
	ctrl->translation_offset = 0;

	ctrl->klm_octowords = htobe16(SNAP_ALIGN_CEIL(klm_entries, 4));

	/*
	 * Going to modify three properties of KLM mkey:
	 *  1. 'free' field: change this mkey from in free to in use
	 *  2. 'len' field: to include the total bytes in iovec
	 *  3. 'start_addr' field: use the address of first element as
	 *       the start_addr of this mkey
	 **/
	ctrl->mkey_mask = htobe64(MLX5_WQE_UMR_CTRL_MKEY_MASK_FREE |
				MLX5_WQE_UMR_CTRL_MKEY_MASK_LEN |
				MLX5_WQE_UMR_CTRL_MKEY_MASK_START_ADDR);
}

static inline void
snap_set_ctrl_seg(struct mlx5_wqe_ctrl_seg *ctrl, uint16_t pi,
			 uint8_t opcode, uint8_t opmod, uint32_t qp_num,
			 uint8_t fm_ce_se, uint8_t ds,
			 uint8_t signature, uint32_t imm)
{
	*(uint32_t *)((void *)ctrl + 8) = 0;
	mlx5dv_set_ctrl_seg(ctrl, pi, opcode, opmod, qp_num,
			fm_ce_se, ds, signature, imm);
}

int snap_dma_q_post_umr_wqe(struct snap_dma_q *q, struct mlx5_klm *klm_mtt,
			int klm_entries, struct snap_indirect_mkey *klm_mkey,
			struct snap_dma_completion *comp, int *n_bb)
{
	struct snap_dv_qp *dv_qp;
	struct mlx5_wqe_ctrl_seg *ctrl, *gen_ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr_ctrl;
	struct mlx5_wqe_mkey_context_seg *mkey;
	union mlx5_wqe_umr_inline_seg *klm;
	int pi, i, umr_wqe_n_bb;
	uint32_t wqe_size, inline_klm_size;
	uint32_t translation_size, to_end;
	uint8_t fm_ce_se = 0;

	if (q->sw_qp.mode == SNAP_DMA_Q_MODE_VERBS)
		return -ENOTSUP;

	if (klm_mtt == NULL || klm_entries == 0)
		return -EINVAL;

	/*
	 * UMR WQE LAYOUT:
	 * -------------------------------------------------------
	 * | gen_ctrl | umr_ctrl | mkey_ctx |   inline klm mtt   |
	 * -------------------------------------------------------
	 *   16bytes    48bytes    64bytes    num_mtt * 16 bytes
	 *
	 * Note: size of inline klm mtt should be aligned to 64 bytes.
	 */
	translation_size = SNAP_ALIGN_CEIL(klm_entries, 4);
	inline_klm_size = translation_size * sizeof(*klm);
	wqe_size = sizeof(*gen_ctrl) + sizeof(*umr_ctrl) +
		sizeof(*mkey) + inline_klm_size;

	umr_wqe_n_bb = round_up(wqe_size, MLX5_SEND_WQE_BB);

	/*
	 * umr wqe only do the modification to klm-mkey,
	 * and there will be one RDMA/GGA-READ/WEIR wqe
	 * followed right after to use this modified klm-key.
	 *
	 * A .readv()/.writev() consider process succeed
	 * only when both umr wqe and RDMA/GGA-READ/WEIR
	 * wqes post succeed.
	 */
	*n_bb = umr_wqe_n_bb + 1;
	if (snap_unlikely(!qp_can_tx(q, *n_bb)))
		return -EAGAIN;

	dv_qp = &q->sw_qp.dv_qp;
	fm_ce_se |= snap_dv_get_cq_update(dv_qp, comp);
	ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);

	pi = dv_qp->pi & (dv_qp->qp.sq.wqe_cnt - 1);
	to_end = (dv_qp->qp.sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	/* sizeof(gen_ctrl) + sizeof(umr_ctrl) == MLX5_SEND_WQE_BB,
	 * so do not need to worry about wqe buffer warp around.
	 * build genenal ctrl segment
	 **/
	gen_ctrl = ctrl;
	snap_set_ctrl_seg(gen_ctrl, dv_qp->pi, MLX5_OPCODE_UMR, 0,
				q->sw_qp.qp->qp_num, fm_ce_se,
				round_up(wqe_size, 16), 0, htobe32(klm_mkey->mkey));

	/* build umr ctrl segment */
	umr_ctrl = (struct mlx5_wqe_umr_ctrl_seg *)(gen_ctrl + 1);
	snap_set_umr_control_seg(umr_ctrl, klm_entries);

	/* build mkey context segment */
	to_end -= MLX5_SEND_WQE_BB;
	if (to_end == 0) { /* wqe buffer wap around */
		mkey = (struct mlx5_wqe_mkey_context_seg *)(dv_qp->qp.sq.buf);
		to_end = dv_qp->qp.sq.wqe_cnt * MLX5_SEND_WQE_BB;
	} else {
		mkey = (struct mlx5_wqe_mkey_context_seg *)(umr_ctrl + 1);
	}
	snap_set_umr_mkey_seg(mkey, klm_mtt, klm_entries);

	/* build inline mtt entires */
	to_end -= MLX5_SEND_WQE_BB;
	if (to_end == 0) { /* wqe buffer wap around */
		klm = (union mlx5_wqe_umr_inline_seg *) (dv_qp->qp.sq.buf);
		to_end = dv_qp->qp.sq.wqe_cnt * MLX5_SEND_WQE_BB;
	} else {
		klm = (union mlx5_wqe_umr_inline_seg *)(mkey + 1);
	}

	for (i = 0; i < klm_entries; i++) {
		snap_set_umr_inline_klm_seg(klm, &klm_mtt[i]);
		/* sizeof(*klm) * 4 == MLX5_SEND_WQE_BB */
		to_end -= sizeof(union mlx5_wqe_umr_inline_seg);
		if (to_end == 0) { /* wqe buffer wap around */
			klm = (union mlx5_wqe_umr_inline_seg *) (dv_qp->qp.sq.buf);
			to_end = dv_qp->qp.sq.wqe_cnt * MLX5_SEND_WQE_BB;
		} else {
			klm = klm + 1;
		}
	}

	/* fill PAD if existing */
	/* PAD entries is to make whole mtt aligned to 64B(MLX5_SEND_WQE_BB),
	 * So it will not happen warp around during fill PAD entries.
	 **/
	for (; i < translation_size; i++) {
		memset(klm, 0, sizeof(*klm));
		klm = klm + 1;
	}

	dv_qp->pi += (umr_wqe_n_bb - 1);

	snap_dv_ring_tx_db(dv_qp, ctrl);
	snap_dv_set_comp(dv_qp, pi, comp, fm_ce_se, umr_wqe_n_bb);

	klm_mkey->addr = klm_mtt[0].address;

	return 0;
}

static inline int do_dv_dma_xfer(struct snap_dma_q *q, void *buf, size_t len,
		uint32_t lkey, uint64_t raddr, uint32_t rkey, int op, int flags,
		struct snap_dma_completion *comp, bool use_fence)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_raddr_seg *rseg;
	struct mlx5_wqe_data_seg *dseg;
	uint16_t comp_idx;
	uint8_t fm_ce_se = 0;

	fm_ce_se |= snap_dv_get_cq_update(dv_qp, comp);
	if (use_fence)
		fm_ce_se |= MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;

	ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg(ctrl, dv_qp->pi, op, 0, q->sw_qp.qp->qp_num,
			    fm_ce_se, 3, 0, 0);

	rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
	rseg->raddr = htobe64((uintptr_t)raddr);
	rseg->rkey  = htobe32(rkey);

	dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
	mlx5dv_set_data_seg(dseg, len, lkey, (intptr_t)buf);

	snap_dv_ring_tx_db(dv_qp, ctrl);

	/* it is better to start dma as soon as possible and do
	 * bookkeeping later
	 **/
	comp_idx = (dv_qp->pi - 1) & (dv_qp->qp.sq.wqe_cnt - 1);
	snap_dv_set_comp(dv_qp, comp_idx, comp, fm_ce_se, 1);
	return 0;
}

static int dv_dma_q_write(struct snap_dma_q *q, void *src_buf, size_t len,
			  uint32_t lkey, uint64_t dstaddr, uint32_t rmkey,
			  struct snap_dma_completion *comp)
{
	return do_dv_dma_xfer(q, src_buf, len, lkey, dstaddr, rmkey,
			MLX5_OPCODE_RDMA_WRITE, 0, comp, false);
}

static int dv_dma_q_read(struct snap_dma_q *q, void *dst_buf, size_t len,
			 uint32_t lkey, uint64_t srcaddr, uint32_t rmkey,
			 struct snap_dma_completion *comp)
{
	return do_dv_dma_xfer(q, dst_buf, len, lkey, srcaddr, rmkey,
			MLX5_OPCODE_RDMA_READ, 0, comp, false);
}

static void snap_use_klm_mkey_done(struct snap_dma_completion *comp, int status)
{
	struct snap_dma_q_io_ctx *io_ctx;
	struct snap_dma_q *q;
	struct snap_dma_completion *orig_comp;

	io_ctx = container_of(comp, struct snap_dma_q_io_ctx, comp);

	q = io_ctx->q;
	orig_comp = (struct snap_dma_completion *)io_ctx->uctx;

	TAILQ_INSERT_HEAD(&q->free_io_ctx, io_ctx, entry);

	if (--orig_comp->count == 0)
		orig_comp->func(orig_comp, status);
}

static inline int snap_iov_to_klm_mtt(struct iovec *iov, int iov_cnt,
			uint32_t mkey, struct mlx5_klm *klm_mtt, size_t *len)
{
	int i;

	/*TODO: dynamically expand klm_mtt array */
	if (iov_cnt > SNAP_DMA_Q_MAX_IOV_CNT) {
		snap_error("iov_cnt:%d is larger than max supportted(%d)\n",
			iov_cnt, SNAP_DMA_Q_MAX_IOV_CNT);
		return -EINVAL;
	}

	for (i = 0; i < iov_cnt; i++) {
		klm_mtt[i].byte_count = iov[i].iov_len;
		klm_mtt[i].mkey = mkey;
		klm_mtt[i].address = (uintptr_t)iov[i].iov_base;

		*len += iov[i].iov_len;
	}

	return 0;
}

/* return NULL if prepare io_ctx failed in any reason,
 * and use 'errno' to pass the actually failure reason.
 */
static struct snap_dma_q_io_ctx*
snap_prepare_io_ctx(struct snap_dma_q *q, struct iovec *iov,
				int iov_cnt, uint32_t rmkey,
				struct snap_dma_completion *comp,
				size_t *len, int *n_bb)
{
	int ret;
	struct snap_dma_q_io_ctx *io_ctx;

	io_ctx = TAILQ_FIRST(&q->free_io_ctx);
	if (!io_ctx) {
		errno = -ENOMEM;
		snap_error("dma_q:%p Out of io_ctx from pool\n", q);
		return NULL;
	}

	TAILQ_REMOVE(&q->free_io_ctx, io_ctx, entry);

	ret = snap_iov_to_klm_mtt(iov, iov_cnt, rmkey, io_ctx->klm_mtt, len);
	if (ret)
		goto insert_back;

	io_ctx->uctx = comp;
	io_ctx->comp.func = snap_use_klm_mkey_done;
	io_ctx->comp.count = 1;

	ret = snap_dma_q_post_umr_wqe(q, io_ctx->klm_mtt, iov_cnt,
				io_ctx->klm_mkey, NULL, n_bb);
	if (ret) {
		snap_error("dma_q:%p post umr wqe failed, ret:%d\n", q, ret);
		goto insert_back;
	}

	return io_ctx;

insert_back:
	TAILQ_INSERT_TAIL(&q->free_io_ctx, io_ctx, entry);
	errno = ret;

	return NULL;
}

static int dv_dma_q_writev(struct snap_dma_q *q, void *src_buf,
				uint32_t lkey, struct iovec *iov, int iov_cnt,
				uint32_t rmkey, struct snap_dma_completion *comp,
				int *n_bb)
{
	size_t len = 0;
	struct snap_indirect_mkey *klm_mkey;
	struct snap_dma_q_io_ctx *io_ctx;

	io_ctx = snap_prepare_io_ctx(q, iov, iov_cnt, rmkey, comp, &len, n_bb);
	if (!io_ctx)
		return errno;

	klm_mkey = io_ctx->klm_mkey;

	return do_dv_dma_xfer(q, src_buf, len, lkey,
			klm_mkey->addr, klm_mkey->mkey,
			MLX5_OPCODE_RDMA_WRITE, 0,
			&io_ctx->comp, true);
}

static int dv_dma_q_readv(struct snap_dma_q *q, void *dst_buf,
				uint32_t lkey, struct iovec *iov, int iov_cnt,
				uint32_t rmkey, struct snap_dma_completion *comp,
				int *n_bb)
{
	size_t len = 0;
	struct snap_indirect_mkey *klm_mkey;
	struct snap_dma_q_io_ctx *io_ctx;

	io_ctx = snap_prepare_io_ctx(q, iov, iov_cnt, rmkey, comp, &len, n_bb);
	if (!io_ctx)
		return errno;

	klm_mkey = io_ctx->klm_mkey;

	return do_dv_dma_xfer(q, dst_buf, len, lkey,
			klm_mkey->addr, klm_mkey->mkey,
			MLX5_OPCODE_RDMA_READ, 0,
			&io_ctx->comp, true);
}


static inline int do_dv_xfer_inline(struct snap_dma_q *q, void *src_buf, size_t len,
				    int op, uint64_t raddr, uint32_t rkey,
				    struct snap_dma_completion *flush_comp, int *n_bb)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_inl_data_seg *dseg;
	struct mlx5_wqe_raddr_seg *rseg;
	uint16_t pi, wqe_size, to_end;
	uint8_t fm_ce_se = 0;
	void *pdata;

	wqe_size = sizeof(*ctrl) + sizeof(*dseg) + len;
	if (op == MLX5_OPCODE_RDMA_WRITE)
		wqe_size += sizeof(*rseg);

	/* if flush_comp is set it means that we are dealing with the zero
	 * length rdma_write op. Check flush_comp instead of length to allow
	 * optimization in the fast path where the flush_comp is always NULL.
	 */
	if (flush_comp)
		wqe_size -= sizeof(*dseg);

	*n_bb = round_up(wqe_size, MLX5_SEND_WQE_BB);
	if (snap_unlikely(!qp_can_tx(q, *n_bb)))
		return -EAGAIN;

	fm_ce_se |= snap_dv_get_cq_update(dv_qp, flush_comp);

	ctrl = (struct mlx5_wqe_ctrl_seg *)snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg(ctrl, dv_qp->pi, op, 0,
			    q->sw_qp.qp->qp_num, fm_ce_se,
			    round_up(wqe_size, 16), 0, 0);

	if (op == MLX5_OPCODE_RDMA_WRITE) {
		rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
		rseg->raddr = htobe64((uintptr_t)raddr);
		rseg->rkey = htobe32(rkey);
		dseg = (struct mlx5_wqe_inl_data_seg *)(rseg + 1);
	} else
		dseg = (struct mlx5_wqe_inl_data_seg *)(ctrl + 1);

	dseg->byte_count = htobe32(len | MLX5_INLINE_SEG);
	pdata = dseg + 1;

	/* handle wrap around, where inline data needs several building blocks */
	pi = dv_qp->pi & (dv_qp->qp.sq.wqe_cnt - 1);
	to_end = (dv_qp->qp.sq.wqe_cnt - pi) * MLX5_SEND_WQE_BB -
		 sizeof(*ctrl) - sizeof(*dseg);
	if (op == MLX5_OPCODE_RDMA_WRITE)
		to_end -= sizeof(*rseg);

	if (snap_unlikely(len > to_end)) {
		memcpy(pdata, src_buf, to_end);
		memcpy(dv_qp->qp.sq.buf, src_buf + to_end, len - to_end);
	} else {
		memcpy(pdata, src_buf, len);
	}

	dv_qp->pi += (*n_bb - 1);

	snap_dv_ring_tx_db(dv_qp, ctrl);

	snap_dv_set_comp(dv_qp, pi, flush_comp, fm_ce_se, *n_bb);
	return 0;
}

static int dv_dma_q_send_completion(struct snap_dma_q *q, void *src_buf,
				    size_t len, int *n_bb)
{
	return do_dv_xfer_inline(q, src_buf, len, MLX5_OPCODE_SEND, 0, 0, NULL, n_bb);
}

static int dv_dma_q_write_short(struct snap_dma_q *q, void *src_buf, size_t len,
				uint64_t dstaddr, uint32_t rmkey, int *n_bb)
{
	return do_dv_xfer_inline(q, src_buf, len, MLX5_OPCODE_RDMA_WRITE,
			dstaddr, rmkey, NULL, n_bb);
}

static inline struct mlx5_cqe64 *snap_dv_get_cqe(struct snap_dv_cq *dv_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	/* note: that the cq_size is known at the compilation time. We pass it
	 * down here so that branch and multiplication will be done at the
	 * compile time during inlining
	 **/
	cqe = (struct mlx5_cqe64 *)(dv_cq->cq.buf + (dv_cq->ci & (dv_cq->cq.cqe_cnt - 1)) *
				    cqe_size);
	return cqe_size == 64 ? cqe : cqe + 1;
}

static inline struct mlx5_cqe64 *snap_dv_poll_cq(struct snap_dv_cq *dv_cq, int cqe_size)
{
	struct mlx5_cqe64 *cqe;

	cqe = snap_dv_get_cqe(dv_cq, cqe_size);

	/* cqe is hw owned */
	if (mlx5dv_get_cqe_owner(cqe) == !(dv_cq->ci & dv_cq->cq.cqe_cnt))
		return NULL;

	/* and must have valid opcode */
	if (mlx5dv_get_cqe_opcode(cqe) == MLX5_CQE_INVALID)
		return NULL;

	dv_cq->ci++;

	snap_debug("ci: %d CQ opcode %d size %d wqe_counter %d scatter32 %d scatter64 %d\n",
		   dv_cq->ci,
		   mlx5dv_get_cqe_opcode(cqe),
		   be32toh(cqe->byte_cnt),
		   be16toh(cqe->wqe_counter),
		   cqe->op_own & MLX5_INLINE_SCATTER_32,
		   cqe->op_own & MLX5_INLINE_SCATTER_64);
	return cqe;
}

static const char *snap_dv_cqe_err_opcode(struct mlx5_err_cqe *ecqe)
{
	uint8_t wqe_err_opcode = be32toh(ecqe->s_wqe_opcode_qpn) >> 24;

	switch (ecqe->op_own >> 4) {
	case MLX5_CQE_REQ_ERR:
		switch (wqe_err_opcode) {
		case MLX5_OPCODE_RDMA_WRITE_IMM:
		case MLX5_OPCODE_RDMA_WRITE:
			return "RDMA_WRITE";
		case MLX5_OPCODE_SEND_IMM:
		case MLX5_OPCODE_SEND:
		case MLX5_OPCODE_SEND_INVAL:
			return "SEND";
		case MLX5_OPCODE_RDMA_READ:
			return "RDMA_READ";
		case MLX5_OPCODE_ATOMIC_CS:
			return "COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_FA:
			return "FETCH_ADD";
		case MLX5_OPCODE_ATOMIC_MASKED_CS:
			return "MASKED_COMPARE_SWAP";
		case MLX5_OPCODE_ATOMIC_MASKED_FA:
			return "MASKED_FETCH_ADD";
		case MLX5_OPCODE_MMO:
			return "GGA_DMA";
		default:
			return "";
			}
	case MLX5_CQE_RESP_ERR:
		return "RECV";
	default:
		return "";
	}
}

static void snap_dv_cqe_err(struct mlx5_cqe64 *cqe)
{
	struct mlx5_err_cqe *ecqe = (struct mlx5_err_cqe *)cqe;
	uint16_t wqe_counter;
	uint32_t qp_num = 0;
	char info[200] = {0};

	wqe_counter = be16toh(ecqe->wqe_counter);
	qp_num = be32toh(ecqe->s_wqe_opcode_qpn) & ((1<<24)-1);

	if (ecqe->syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR) {
		snap_debug("QP 0x%x wqe[%d] is flushed\n", qp_num, wqe_counter);
		return;
	}

	switch (ecqe->syndrome) {
	case MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR:
		snprintf(info, sizeof(info), "Local length");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR:
		snprintf(info, sizeof(info), "Local QP operation");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_PROT_ERR:
		snprintf(info, sizeof(info), "Local protection");
		break;
	case MLX5_CQE_SYNDROME_WR_FLUSH_ERR:
		snprintf(info, sizeof(info), "WR flushed because QP in error state");
		break;
	case MLX5_CQE_SYNDROME_MW_BIND_ERR:
		snprintf(info, sizeof(info), "Memory window bind");
		break;
	case MLX5_CQE_SYNDROME_BAD_RESP_ERR:
		snprintf(info, sizeof(info), "Bad response");
		break;
	case MLX5_CQE_SYNDROME_LOCAL_ACCESS_ERR:
		snprintf(info, sizeof(info), "Local access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_INVAL_REQ_ERR:
		snprintf(info, sizeof(info), "Invalid request");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ACCESS_ERR:
		snprintf(info, sizeof(info), "Remote access");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_OP_ERR:
		snprintf(info, sizeof(info), "Remote QP");
		break;
	case MLX5_CQE_SYNDROME_TRANSPORT_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Transport retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_RNR_RETRY_EXC_ERR:
		snprintf(info, sizeof(info), "Receive-no-ready retry count exceeded");
		break;
	case MLX5_CQE_SYNDROME_REMOTE_ABORTED_ERR:
		snprintf(info, sizeof(info), "Remote side aborted");
		break;
	default:
		snprintf(info, sizeof(info), "Generic");
		break;
	}
	snap_error("Error on QP 0x%x wqe[%03d]: %s (synd 0x%x vend 0x%x) opcode %s\n",
		   qp_num, wqe_counter, info, ecqe->syndrome, ecqe->vendor_err_synd,
		   snap_dv_cqe_err_opcode(ecqe));
}

static inline int dv_dma_q_progress_tx(struct snap_dma_q *q)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_cqe64 *cqe;
	struct snap_dma_completion *comp;
	uint16_t comp_idx;
	int n;
	uint32_t sq_mask;

	n = 0;
	sq_mask = dv_qp->qp.sq.wqe_cnt - 1;
	do {
		cqe = snap_dv_poll_cq(&q->sw_qp.dv_tx_cq, SNAP_DMA_Q_TX_CQE_SIZE);
		if (!cqe)
			break;

		if (snap_unlikely(mlx5dv_get_cqe_opcode(cqe) != MLX5_CQE_REQ))
			snap_dv_cqe_err(cqe);

		n++;
		comp_idx = be16toh(cqe->wqe_counter) & sq_mask;
		q->tx_available += dv_qp->comps[comp_idx].n_outstanding;
		comp = dv_qp->comps[comp_idx].comp;

		if (comp && --comp->count == 0)
			comp->func(comp, mlx5dv_get_cqe_opcode(cqe));

	} while (n < SNAP_DMA_MAX_TX_COMPLETIONS);

	return n;
}

static inline int dv_dma_q_progress_rx(struct snap_dma_q *q)
{
	struct mlx5_cqe64 *cqe;
	int n, ri;
	int op;
	uint32_t rq_mask;

	rq_mask = q->sw_qp.dv_qp.qp.rq.wqe_cnt - 1;
	n = 0;
	do {
		cqe = snap_dv_poll_cq(&q->sw_qp.dv_rx_cq, SNAP_DMA_Q_RX_CQE_SIZE);
		if (!cqe)
			break;

		op = mlx5dv_get_cqe_opcode(cqe);
		if (snap_unlikely(op != MLX5_CQE_RESP_SEND &&
				  op != MLX5_CQE_RESP_SEND_IMM)) {
			snap_dv_cqe_err(cqe);
			return n;
		}

		snap_memory_cpu_load_fence();

		n++;
		/* optimize for NVMe where SQE is 64 bytes and will always
		 * be scattered
		 **/
		if (snap_likely(cqe->op_own & MLX5_INLINE_SCATTER_64)) {
			__builtin_prefetch(cqe - 1);
			q->rx_cb(q, cqe - 1, be32toh(cqe->byte_cnt), cqe->imm_inval_pkey);
		} else if (cqe->op_own & MLX5_INLINE_SCATTER_32) {
			q->rx_cb(q, cqe, be32toh(cqe->byte_cnt), cqe->imm_inval_pkey);
		} else {
			ri = be16toh(cqe->wqe_counter) & rq_mask;
			__builtin_prefetch(q->sw_qp.rx_buf + ri * q->rx_elem_size);
			q->rx_cb(q, q->sw_qp.rx_buf + ri * q->rx_elem_size,
				 be32toh(cqe->byte_cnt), cqe->imm_inval_pkey);
		}

	} while (n < SNAP_DMA_MAX_RX_COMPLETIONS);

	if (n == 0)
		return 0;

	q->sw_qp.dv_qp.ci += n;
	snap_dv_ring_rx_db(&q->sw_qp.dv_qp);
	return n;
}

static struct snap_dma_q_ops dv_ops = {
	.write           = dv_dma_q_write,
	.writev          = dv_dma_q_writev,
	.write_short     = dv_dma_q_write_short,
	.read            = dv_dma_q_read,
	.readv           = dv_dma_q_readv,
	.send_completion = dv_dma_q_send_completion,
	.progress_tx     = dv_dma_q_progress_tx,
	.progress_rx     = dv_dma_q_progress_rx,
};

/* GGA */
__attribute__((unused)) static void dump_gga_wqe(int op, uint32_t *wqe)
{
	int i;

	printf("%s op %d wqe:\n", __func__, op);

	for (i = 0; i < 16; i += 4)
		printf("%08X %08X %08X %08X\n",
			ntohl(wqe[i]), ntohl(wqe[i + 1]),
			ntohl(wqe[i + 2]), ntohl(wqe[i + 3]));
}

static inline int do_gga_xfer(struct snap_dma_q *q, uint64_t saddr, size_t len,
			      uint32_t s_lkey, uint64_t daddr, uint32_t d_lkey,
			      struct snap_dma_completion *comp, bool use_fence)
{
	struct snap_dv_qp *dv_qp = &q->sw_qp.dv_qp;
	struct mlx5_dma_wqe *gga_wqe;
	/* struct mlx5_wqe_ctrl_seg changed to packed(4),
	 * and struct mlx5_dma_wqe is use default packed attribute, which is 8.
	 * in order to fix the compile issue on UB OS, make `ctrl` to void*,
	 * and convert it to struct mlx5_wqe_ctrl_seg * when it is needed.
	 */
	void *ctrl;
	uint16_t comp_idx;
	uint8_t fm_ce_se = 0;

	comp_idx = dv_qp->pi & (dv_qp->qp.sq.wqe_cnt - 1);

	fm_ce_se |= snap_dv_get_cq_update(dv_qp, comp);
	if (use_fence)
		fm_ce_se |= MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE;

	ctrl = snap_dv_get_wqe_bb(dv_qp);
	snap_set_ctrl_seg((struct mlx5_wqe_ctrl_seg *)ctrl, dv_qp->pi,
			    MLX5_OPCODE_MMO, MLX5_OPC_MOD_MMO_DMA,
			    q->sw_qp.qp->qp_num, fm_ce_se,
			    4, 0, 0);

	gga_wqe = (struct mlx5_dma_wqe *)ctrl;
	gga_wqe->gga_ctrl2 = 0;
	gga_wqe->opaque_lkey = htobe32(dv_qp->opaque_mr->lkey);
	gga_wqe->opaque_vaddr = htobe64((uint64_t)&dv_qp->opaque_buf[comp_idx]);

	mlx5dv_set_data_seg(&gga_wqe->gather, len, s_lkey, saddr);
	mlx5dv_set_data_seg(&gga_wqe->scatter, len, d_lkey, daddr);

	snap_dv_ring_tx_db(dv_qp, (struct mlx5_wqe_ctrl_seg *)ctrl);

	snap_dv_set_comp(dv_qp, comp_idx, comp, fm_ce_se, 1);
	return 0;
}

static int gga_dma_q_write(struct snap_dma_q *q, void *src_buf, size_t len,
			  uint32_t lkey, uint64_t dstaddr, uint32_t rmkey,
			  struct snap_dma_completion *comp)
{
	return do_gga_xfer(q, (uint64_t)src_buf, len, lkey,
			dstaddr, rmkey, comp, false);
}

static int gga_dma_q_read(struct snap_dma_q *q, void *dst_buf, size_t len,
			 uint32_t lkey, uint64_t srcaddr, uint32_t rmkey,
			 struct snap_dma_completion *comp)
{
	return do_gga_xfer(q, srcaddr, len, rmkey,
			(uint64_t)dst_buf, lkey, comp, false);
}

static int gga_dma_q_writev(struct snap_dma_q *q, void *src_buf, uint32_t lkey,
			struct iovec *iov, int iov_cnt, uint32_t rmkey,
			struct snap_dma_completion *comp, int *n_bb)
{
	size_t len = 0;
	struct snap_indirect_mkey *klm_mkey;
	struct snap_dma_q_io_ctx *io_ctx;

	io_ctx = snap_prepare_io_ctx(q, iov, iov_cnt, rmkey, comp, &len, n_bb);
	if (!io_ctx)
		return errno;

	klm_mkey = io_ctx->klm_mkey;

	return do_gga_xfer(q, (uint64_t)src_buf, len, lkey,
			klm_mkey->addr, klm_mkey->mkey, &io_ctx->comp, true);
}

static int gga_dma_q_readv(struct snap_dma_q *q, void *dst_buf, uint32_t lkey,
			struct iovec *iov, int iov_cnt, uint32_t rmkey,
			struct snap_dma_completion *comp, int *n_bb)
{
	size_t len = 0;
	struct snap_indirect_mkey *klm_mkey;
	struct snap_dma_q_io_ctx *io_ctx;

	io_ctx = snap_prepare_io_ctx(q, iov, iov_cnt, rmkey, comp, &len, n_bb);
	if (!io_ctx)
		return errno;

	klm_mkey = io_ctx->klm_mkey;

	return do_gga_xfer(q, klm_mkey->addr, len, klm_mkey->mkey,
			(uint64_t)dst_buf, lkey, &io_ctx->comp, true);
}

static struct snap_dma_q_ops gga_ops = {
	.write           = gga_dma_q_write,
	.writev          = gga_dma_q_writev,
	.write_short     = dv_dma_q_write_short,
	.read            = gga_dma_q_read,
	.readv           = gga_dma_q_readv,
	.send_completion = dv_dma_q_send_completion,
	.progress_tx     = dv_dma_q_progress_tx,
	.progress_rx     = dv_dma_q_progress_rx,
};