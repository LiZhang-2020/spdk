/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "spdk/log.h"

#include "common/lib/test_env.c"

#include "nvme/nvme_ctrlr.c"
#include "nvme/nvme_quirks.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

struct nvme_driver *g_spdk_nvme_driver = &_g_nvme_driver;

struct spdk_nvme_registers g_ut_nvme_regs = {};
typedef void (*set_reg_cb)(void);
set_reg_cb g_set_reg_cb;

__thread int    nvme_thread_ioq_index = -1;

uint32_t set_size = 1;

int set_status_cpl = -1;

DEFINE_STUB(nvme_ctrlr_cmd_set_host_id, int,
	    (struct spdk_nvme_ctrlr *ctrlr, void *host_id, uint32_t host_id_size,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB_V(nvme_ns_set_identify_data, (struct spdk_nvme_ns *ns));
DEFINE_STUB_V(nvme_ns_set_id_desc_list_data, (struct spdk_nvme_ns *ns));
DEFINE_STUB_V(nvme_ns_free_zns_specific_data, (struct spdk_nvme_ns *ns));
DEFINE_STUB_V(nvme_ns_free_iocs_specific_data, (struct spdk_nvme_ns *ns));
DEFINE_STUB(nvme_ns_has_supported_iocs_specific_data, bool, (struct spdk_nvme_ns *ns), false);
DEFINE_STUB_V(nvme_qpair_abort_reqs, (struct spdk_nvme_qpair *qpair, uint32_t dnr));
DEFINE_STUB(spdk_nvme_poll_group_remove, int, (struct spdk_nvme_poll_group *group,
		struct spdk_nvme_qpair *qpair), 0);
DEFINE_STUB_V(nvme_io_msg_ctrlr_update, (struct spdk_nvme_ctrlr *ctrlr));
DEFINE_STUB(nvme_io_msg_process, int, (struct spdk_nvme_ctrlr *ctrlr), 0);
DEFINE_STUB(nvme_transport_ctrlr_reserve_cmb, int, (struct spdk_nvme_ctrlr *ctrlr), 0);
DEFINE_STUB(spdk_nvme_ctrlr_cmd_security_receive, int, (struct spdk_nvme_ctrlr *ctrlr,
		uint8_t secp, uint16_t spsp, uint8_t nssf, void *payload,
		uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvme_ctrlr_cmd_security_send, int, (struct spdk_nvme_ctrlr *ctrlr,
		uint8_t secp, uint16_t spsp, uint8_t nssf, void *payload,
		uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	return NULL;
}

int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_destruct_finish(ctrlr);

	return 0;
}

int
nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 4);
	*(uint32_t *)((uintptr_t)&g_ut_nvme_regs + offset) = value;
	if (g_set_reg_cb) {
		g_set_reg_cb();
	}
	return 0;
}

int
nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 8);
	*(uint64_t *)((uintptr_t)&g_ut_nvme_regs + offset) = value;
	if (g_set_reg_cb) {
		g_set_reg_cb();
	}
	return 0;
}

int
nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 4);
	*value = *(uint32_t *)((uintptr_t)&g_ut_nvme_regs + offset);
	return 0;
}

int
nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 8);
	*value = *(uint64_t *)((uintptr_t)&g_ut_nvme_regs + offset);
	return 0;
}

uint32_t
nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return UINT32_MAX;
}

uint16_t
nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return 1;
}

void *
nvme_transport_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	return NULL;
}

int
nvme_transport_ctrlr_unmap_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

void *
nvme_transport_ctrlr_map_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	return NULL;
}

int
nvme_transport_ctrlr_unmap_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_qpair *qpair;

	qpair = calloc(1, sizeof(*qpair));
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	qpair->ctrlr = ctrlr;
	qpair->id = qid;
	qpair->qprio = opts->qprio;

	return qpair;
}

int
nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	free(qpair);
	return 0;
}

void
nvme_transport_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
}

int
nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

void
nvme_transport_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
}

void
nvme_transport_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
}

int
nvme_driver_init(void)
{
	return 0;
}

int nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		    struct spdk_nvme_ctrlr *ctrlr,
		    enum spdk_nvme_qprio qprio,
		    uint32_t num_requests)
{
	qpair->id = id;
	qpair->qprio = qprio;
	qpair->ctrlr = ctrlr;

	return 0;
}

static struct spdk_nvme_cpl fake_cpl = {};
static enum spdk_nvme_generic_command_status_code set_status_code = SPDK_NVME_SC_SUCCESS;

static void
fake_cpl_sc(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl.status.sc = set_status_code;
	cb_fn(cb_arg, &fake_cpl);
}

static uint32_t g_ut_cdw11;

int
spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, uint32_t cdw12, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	g_ut_cdw11 = cdw11;
	return 0;
}

int
spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size,
				 uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

int
spdk_nvme_ctrlr_cmd_get_log_page_ext(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				     uint32_t nsid, void *payload, uint32_t payload_size,
				     uint64_t offset, uint32_t cdw10, uint32_t cdw11,
				     uint32_t cdw14, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST);

	/*
	 * For the purposes of this unit test, we don't need to bother emulating request submission.
	 */

	return 0;
}

static int32_t g_wait_for_completion_return_val;

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return g_wait_for_completion_return_val;
}

void
nvme_qpair_complete_error_reqs(struct spdk_nvme_qpair *qpair)
{
}


void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_completion_poll_status	*status = arg;
	/* This should not happen it test env since this callback is always called
	 * before wait_for_completion_* while this field can only be set to true in
	 * wait_for_completion_* functions */
	CU_ASSERT(status->timed_out == false);

	status->cpl = *cpl;
	status->done = true;
}

static struct nvme_completion_poll_status *g_failed_status;

int
nvme_wait_for_completion_robust_lock_timeout(
	struct spdk_nvme_qpair *qpair,
	struct nvme_completion_poll_status *status,
	pthread_mutex_t *robust_mutex,
	uint64_t timeout_in_usecs)
{
	if (spdk_nvme_qpair_process_completions(qpair, 0) < 0) {
		g_failed_status = status;
		status->timed_out = true;
		return -1;
	}

	status->done = true;
	if (set_status_cpl == 1) {
		status->cpl.status.sc = 1;
	}
	return spdk_nvme_cpl_is_error(&status->cpl) ? -EIO : 0;
}

int
nvme_wait_for_completion_robust_lock(
	struct spdk_nvme_qpair *qpair,
	struct nvme_completion_poll_status *status,
	pthread_mutex_t *robust_mutex)
{
	return nvme_wait_for_completion_robust_lock_timeout(qpair, status, robust_mutex, 0);
}

int
nvme_wait_for_completion(struct spdk_nvme_qpair *qpair,
			 struct nvme_completion_poll_status *status)
{
	return nvme_wait_for_completion_robust_lock_timeout(qpair, status, NULL, 0);
}

int
nvme_wait_for_completion_timeout(struct spdk_nvme_qpair *qpair,
				 struct nvme_completion_poll_status *status,
				 uint64_t timeout_in_usecs)
{
	return nvme_wait_for_completion_robust_lock_timeout(qpair, status, NULL, timeout_in_usecs);
}

int
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_feat_async_event_configuration config, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

static uint32_t *g_active_ns_list = NULL;
static uint32_t g_active_ns_list_length = 0;
static struct spdk_nvme_ctrlr_data *g_cdata = NULL;

int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			uint8_t csi, void *payload, size_t payload_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	memset(payload, 0, payload_size);
	if (cns == SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST) {
		uint32_t count = 0;
		uint32_t i = 0;
		struct spdk_nvme_ns_list *ns_list = (struct spdk_nvme_ns_list *)payload;

		if (g_active_ns_list == NULL) {
			for (i = 1; i <= ctrlr->num_ns; i++) {
				if (i <= nsid) {
					continue;
				}

				ns_list->ns_list[count++] = i;
				if (count == SPDK_COUNTOF(ns_list->ns_list)) {
					break;
				}
			}
		} else {
			for (i = 0; i < g_active_ns_list_length; i++) {
				uint32_t cur_nsid = g_active_ns_list[i];
				if (cur_nsid <= nsid) {
					continue;
				}

				ns_list->ns_list[count++] = cur_nsid;
				if (count == SPDK_COUNTOF(ns_list->ns_list)) {
					break;
				}
			}
		}
	} else if (cns == SPDK_NVME_IDENTIFY_CTRLR) {
		if (g_cdata) {
			memcpy(payload, g_cdata, sizeof(*g_cdata));
		}
	}

	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_get_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	CU_ASSERT(0);
	return -1;
}

int
nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_cmd_cb cb_fn,
			 void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, struct spdk_nvme_format *format,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
spdk_nvme_ctrlr_cmd_directive_send(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				   uint32_t doper, uint32_t dtype, uint32_t dspec,
				   void *payload, uint32_t payload_size, uint32_t cdw12,
				   uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
spdk_nvme_ctrlr_cmd_directive_receive(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
				      uint32_t doper, uint32_t dtype, uint32_t dspec,
				      void *payload, uint32_t payload_size, uint32_t cdw12,
				      uint32_t cdw13, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_fw_commit(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_fw_commit *fw_commit,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	CU_ASSERT(fw_commit->ca == SPDK_NVME_FW_COMMIT_REPLACE_IMG);
	if (fw_commit->fs == 0) {
		return -1;
	}
	set_status_cpl = 1;
	if (ctrlr->is_resetting == true) {
		set_status_cpl = 0;
	}
	return 0;
}

int
nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
				 uint32_t size, uint32_t offset, void *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	if ((size != 0 && payload == NULL) || (size == 0 && payload != NULL)) {
		return -1;
	}
	CU_ASSERT(offset == 0);
	return 0;
}

void
nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
}

int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint32_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_ns_update(struct spdk_nvme_ns *ns)
{
	return 0;
}

void
spdk_pci_device_detach(struct spdk_pci_device *device)
{
}

#define DECLARE_AND_CONSTRUCT_CTRLR()	\
	struct spdk_nvme_ctrlr	ctrlr = {};	\
	struct spdk_nvme_qpair	adminq = {};	\
	struct nvme_request	req;		\
						\
	STAILQ_INIT(&adminq.free_req);		\
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);	\
	ctrlr.adminq = &adminq;

static void
test_nvme_ctrlr_init_en_1_rdy_0(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 1, CSTS.RDY = 0
	 */
	g_ut_nvme_regs.cc.bits.en = 1;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1);

	/*
	 * Transition to CSTS.RDY = 1.
	 * init() should set CC.EN = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Transition to CSTS.RDY = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	/*
	 * Transition to CC.EN = 1
	 */
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_1_rdy_1(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 1, CSTS.RDY = 1
	 * init() should set CC.EN = 0.
	 */
	g_ut_nvme_regs.cc.bits.en = 1;
	g_ut_nvme_regs.csts.bits.rdy = 1;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Transition to CSTS.RDY = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	/*
	 * Transition to CC.EN = 1
	 */
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_rr(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Default round robin enabled
	 */
	g_ut_nvme_regs.cap.bits.ams = 0x0;
	ctrlr.cap = g_ut_nvme_regs.cap;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to default round robin arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_wrr(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Weighted round robin enabled
	 */
	g_ut_nvme_regs.cap.bits.ams = SPDK_NVME_CAP_AMS_WRR;
	ctrlr.cap = g_ut_nvme_regs.cap;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_WRR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_WRR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to weighted round robin arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_WRR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_WRR);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}
static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_vs(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Default round robin enabled
	 */
	g_ut_nvme_regs.cap.bits.ams = SPDK_NVME_CAP_AMS_VS;
	ctrlr.cap = g_ut_nvme_regs.cap;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_VS);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_VS);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to vendor specific arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_VS);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_VS);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);

	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_1(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 1
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 1;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);

	/*
	 * Transition to CSTS.RDY = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	/*
	 * Transition to CC.EN = 1
	 */
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
setup_qpairs(struct spdk_nvme_ctrlr *ctrlr, uint32_t num_io_queues)
{
	uint32_t i;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(ctrlr) == 0);

	ctrlr->page_size = 0x1000;
	ctrlr->opts.num_io_queues = num_io_queues;
	ctrlr->free_io_qids = spdk_bit_array_create(num_io_queues + 1);
	SPDK_CU_ASSERT_FATAL(ctrlr->free_io_qids != NULL);

	spdk_bit_array_clear(ctrlr->free_io_qids, 0);
	for (i = 1; i <= num_io_queues; i++) {
		spdk_bit_array_set(ctrlr->free_io_qids, i);
	}
}

static void
cleanup_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_destruct(ctrlr);
}

static void
test_alloc_io_qpair_rr_1(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0;

	setup_qpairs(&ctrlr, 1);

	/*
	 * Fake to simulate the controller with default round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_RR;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));

	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	/* Only 1 I/O qpair was allocated, so this should fail */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0) == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Now that the qpair has been returned to the free list,
	 * we should be able to allocate it again.
	 */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/* Only 0 qprio is acceptable for default round robin arbitration mechanism */
	opts.qprio = 1;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	opts.qprio = 2;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	opts.qprio = 3;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	/* Only 0 ~ 3 qprio is acceptable */
	opts.qprio = 4;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts)) == NULL);

	cleanup_qpairs(&ctrlr);
}

static void
test_alloc_io_qpair_wrr_1(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0, *q1;

	setup_qpairs(&ctrlr, 2);

	/*
	 * Fake to simulate the controller with weighted round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_WRR;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));

	/*
	 * Allocate 2 qpairs and free them
	 */
	opts.qprio = 0;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);

	opts.qprio = 1;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Allocate 2 qpairs and free them in the reverse order
	 */
	opts.qprio = 2;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 2);

	opts.qprio = 3;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 3);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);

	/* Only 0 ~ 3 qprio is acceptable */
	opts.qprio = 4;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts)) == NULL);

	cleanup_qpairs(&ctrlr);
}

static void
test_alloc_io_qpair_wrr_2(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0, *q1, *q2, *q3;

	setup_qpairs(&ctrlr, 4);

	/*
	 * Fake to simulate the controller with weighted round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_WRR;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));

	opts.qprio = 0;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);

	opts.qprio = 1;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);

	opts.qprio = 2;
	q2 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q2 != NULL);
	SPDK_CU_ASSERT_FATAL(q2->qprio == 2);

	opts.qprio = 3;
	q3 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q3 != NULL);
	SPDK_CU_ASSERT_FATAL(q3->qprio == 3);

	/* Only 4 I/O qpairs was allocated, so this should fail */
	opts.qprio = 0;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts)) == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q3) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q2) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Now that the qpair has been returned to the free list,
	 * we should be able to allocate it again.
	 *
	 * Allocate 4 I/O qpairs and half of them with same qprio.
	 */
	opts.qprio = 1;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 1);

	opts.qprio = 1;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);

	opts.qprio = 3;
	q2 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q2 != NULL);
	SPDK_CU_ASSERT_FATAL(q2->qprio == 3);

	opts.qprio = 3;
	q3 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q3 != NULL);
	SPDK_CU_ASSERT_FATAL(q3->qprio == 3);

	/*
	 * Free all I/O qpairs in reverse order
	 */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q2) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q3) == 0);

	cleanup_qpairs(&ctrlr);
}

bool g_connect_qpair_called = false;
int g_connect_qpair_return_code = 0;
int nvme_transport_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	g_connect_qpair_called = true;
	return g_connect_qpair_return_code;
}

static void
test_spdk_nvme_ctrlr_reconnect_io_qpair(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct spdk_nvme_qpair	qpair = {};
	int rc;

	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);

	/* Various states of controller disconnect. */
	qpair.id = 1;
	qpair.ctrlr = &ctrlr;
	ctrlr.is_removed = 1;
	ctrlr.is_failed = 0;
	ctrlr.is_resetting = 0;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(rc == -ENODEV)

	ctrlr.is_removed = 0;
	ctrlr.is_failed = 1;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(rc == -ENXIO)

	ctrlr.is_failed = 0;
	ctrlr.is_resetting = 1;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(rc == -EAGAIN)

	/* Confirm precedence for controller states: removed > resetting > failed */
	ctrlr.is_removed = 1;
	ctrlr.is_failed = 1;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(rc == -ENODEV)

	ctrlr.is_removed = 0;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(rc == -EAGAIN)

	ctrlr.is_resetting = 0;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(rc == -ENXIO)

	/* qpair not failed. Make sure we don't call down to the transport */
	ctrlr.is_failed = 0;
	qpair.state = NVME_QPAIR_CONNECTED;
	g_connect_qpair_called = false;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(g_connect_qpair_called == false);
	CU_ASSERT(rc == 0)

	/* transport qpair is failed. make sure we call down to the transport */
	qpair.state = NVME_QPAIR_DISCONNECTED;
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(&qpair);
	CU_ASSERT(g_connect_qpair_called == true);
	CU_ASSERT(rc == 0)

	CU_ASSERT(pthread_mutex_destroy(&ctrlr.ctrlr_lock) == 0);
}

static void
test_nvme_ctrlr_fail(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.opts.num_io_queues = 0;
	nvme_ctrlr_fail(&ctrlr, false);

	CU_ASSERT(ctrlr.is_failed == true);
}

static void
test_nvme_ctrlr_construct_intel_support_log_page_list(void)
{
	bool	res;
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_intel_log_page_directory	payload = {};
	struct spdk_pci_id				pci_id = {};

	/* Get quirks for a device with all 0 vendor/device id */
	ctrlr.quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT(ctrlr.quirks == 0);

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);

	/* Set the vendor to Intel, but provide no device id */
	pci_id.class_id = SPDK_PCI_CLASS_NVME;
	ctrlr.cdata.vid = pci_id.vendor_id = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 1;
	ctrlr.quirks = nvme_get_quirks(&pci_id);
	memset(ctrlr.log_page_supported, 0, sizeof(ctrlr.log_page_supported));

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
	CU_ASSERT(res == false);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_SMART);
	CU_ASSERT(res == false);

	/* set valid vendor id, device id and sub device id */
	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 0;
	pci_id.vendor_id = SPDK_PCI_VID_INTEL;
	pci_id.device_id = 0x0953;
	pci_id.subvendor_id = SPDK_PCI_VID_INTEL;
	pci_id.subdevice_id = 0x3702;
	ctrlr.quirks = nvme_get_quirks(&pci_id);
	memset(ctrlr.log_page_supported, 0, sizeof(ctrlr.log_page_supported));

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_SMART);
	CU_ASSERT(res == false);
}

static void
test_nvme_ctrlr_set_supported_features(void)
{
	bool	res;
	struct spdk_nvme_ctrlr			ctrlr = {};

	/* set a invalid vendor id */
	ctrlr.cdata.vid = 0xFFFF;
	nvme_ctrlr_set_supported_features(&ctrlr);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_FEAT_ARBITRATION);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_INTEL_FEAT_MAX_LBA);
	CU_ASSERT(res == false);

	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	nvme_ctrlr_set_supported_features(&ctrlr);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_FEAT_ARBITRATION);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_INTEL_FEAT_MAX_LBA);
	CU_ASSERT(res == true);
}

static void
test_ctrlr_get_default_ctrlr_opts(void)
{
	struct spdk_nvme_ctrlr_opts opts = {};

	CU_ASSERT(spdk_uuid_parse(&g_spdk_nvme_driver->default_extended_host_id,
				  "e53e9258-c93b-48b5-be1a-f025af6d232a") == 0);

	memset(&opts, 0, sizeof(opts));

	/* set a smaller opts_size */
	CU_ASSERT(sizeof(opts) > 8);
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, 8);
	CU_ASSERT_EQUAL(opts.num_io_queues, DEFAULT_MAX_IO_QUEUES);
	CU_ASSERT_TRUE(opts.use_cmb_sqs);
	/* check below fields are not initialized by default value */
	CU_ASSERT_EQUAL(opts.arb_mechanism, 0);
	CU_ASSERT_EQUAL(opts.keep_alive_timeout_ms, 0);
	CU_ASSERT_EQUAL(opts.io_queue_size, 0);
	CU_ASSERT_EQUAL(opts.io_queue_requests, 0);
	for (int i = 0; i < 8; i++) {
		CU_ASSERT(opts.host_id[i] == 0);
	}
	for (int i = 0; i < 16; i++) {
		CU_ASSERT(opts.extended_host_id[i] == 0);
	}
	CU_ASSERT(strlen(opts.hostnqn) == 0);
	CU_ASSERT(strlen(opts.src_addr) == 0);
	CU_ASSERT(strlen(opts.src_svcid) == 0);
	CU_ASSERT_EQUAL(opts.admin_timeout_ms, 0);

	/* set a consistent opts_size */
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
	CU_ASSERT_EQUAL(opts.num_io_queues, DEFAULT_MAX_IO_QUEUES);
	CU_ASSERT_TRUE(opts.use_cmb_sqs);
	CU_ASSERT_EQUAL(opts.arb_mechanism, SPDK_NVME_CC_AMS_RR);
	CU_ASSERT_EQUAL(opts.keep_alive_timeout_ms, 10 * 1000);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	CU_ASSERT_EQUAL(opts.io_queue_requests, DEFAULT_IO_QUEUE_REQUESTS);
	for (int i = 0; i < 8; i++) {
		CU_ASSERT(opts.host_id[i] == 0);
	}
	CU_ASSERT_STRING_EQUAL(opts.hostnqn,
			       "nqn.2014-08.org.nvmexpress:uuid:e53e9258-c93b-48b5-be1a-f025af6d232a");
	CU_ASSERT(memcmp(opts.extended_host_id, &g_spdk_nvme_driver->default_extended_host_id,
			 sizeof(opts.extended_host_id)) == 0);
	CU_ASSERT(strlen(opts.src_addr) == 0);
	CU_ASSERT(strlen(opts.src_svcid) == 0);
	CU_ASSERT_EQUAL(opts.admin_timeout_ms, NVME_MAX_ADMIN_TIMEOUT_IN_SECS * 1000);
}

static void
test_ctrlr_get_default_io_qpair_opts(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_io_qpair_opts opts = {};

	memset(&opts, 0, sizeof(opts));

	/* set a smaller opts_size */
	ctrlr.opts.io_queue_size = DEFAULT_IO_QUEUE_SIZE;
	CU_ASSERT(sizeof(opts) > 8);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, 8);
	CU_ASSERT_EQUAL(opts.qprio, SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	/* check below field is not initialized by default value */
	CU_ASSERT_EQUAL(opts.io_queue_requests, 0);

	/* set a consistent opts_size */
	ctrlr.opts.io_queue_size = DEFAULT_IO_QUEUE_SIZE;
	ctrlr.opts.io_queue_requests = DEFAULT_IO_QUEUE_REQUESTS;
	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));
	CU_ASSERT_EQUAL(opts.qprio, SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	CU_ASSERT_EQUAL(opts.io_queue_requests, DEFAULT_IO_QUEUE_REQUESTS);
}

#if 0 /* TODO: move to PCIe-specific unit test */
static void
test_nvme_ctrlr_alloc_cmb(void)
{
	int			rc;
	uint64_t		offset;
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.cmb_size = 0x1000000;
	ctrlr.cmb_current_offset = 0x100;
	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x200, 0x1000, &offset);
	CU_ASSERT(rc == 0);
	CU_ASSERT(offset == 0x1000);
	CU_ASSERT(ctrlr.cmb_current_offset == 0x1200);

	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x800, 0x1000, &offset);
	CU_ASSERT(rc == 0);
	CU_ASSERT(offset == 0x2000);
	CU_ASSERT(ctrlr.cmb_current_offset == 0x2800);

	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x800000, 0x100000, &offset);
	CU_ASSERT(rc == 0);
	CU_ASSERT(offset == 0x100000);
	CU_ASSERT(ctrlr.cmb_current_offset == 0x900000);

	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x8000000, 0x1000, &offset);
	CU_ASSERT(rc == -1);
}
#endif

static void
test_spdk_nvme_ctrlr_update_firmware(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	void *payload = NULL;
	int point_payload = 1;
	int slot = 0;
	int ret = 0;
	struct spdk_nvme_status status;
	enum spdk_nvme_fw_commit_action commit_action = SPDK_NVME_FW_COMMIT_REPLACE_IMG;

	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);

	/* Set invalid size check function return value */
	set_size = 5;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* When payload is NULL but set_size < min_page_size */
	set_size = 4;
	ctrlr.min_page_size = 5;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* When payload not NULL but min_page_size is 0 */
	set_size = 4;
	ctrlr.min_page_size = 0;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* Check firmware image download when payload not NULL and min_page_size not 0 , status.cpl value is 1 */
	set_status_cpl = 1;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -ENXIO);

	/* Check firmware image download and set status.cpl value is 0 */
	set_status_cpl = 0;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* Check firmware commit */
	ctrlr.is_resetting = false;
	set_status_cpl = 0;
	slot = 1;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -ENXIO);

	/* Set size check firmware download and firmware commit */
	ctrlr.is_resetting = true;
	set_status_cpl = 0;
	slot = 1;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == 0);

	/* nvme_wait_for_completion returns an error */
	g_wait_for_completion_return_val = -1;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -ENXIO);
	CU_ASSERT(g_failed_status != NULL);
	CU_ASSERT(g_failed_status->timed_out == true);
	/* status should be freed by callback, which is not triggered in test env.
	   Store status to global variable and free it manually.
	   If spdk_nvme_ctrlr_update_firmware changes its behaviour and frees the status
	   itself, we'll get a double free here.. */
	free(g_failed_status);
	g_failed_status = NULL;
	g_wait_for_completion_return_val = 0;

	CU_ASSERT(pthread_mutex_destroy(&ctrlr.ctrlr_lock) == 0);
	set_status_cpl = 0;
}

int
nvme_ctrlr_cmd_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr, uint64_t prp1, uint64_t prp2,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_sc(cb_fn, cb_arg);
	return 0;
}

static void
test_spdk_nvme_ctrlr_doorbell_buffer_config(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	int ret = -1;

	ctrlr.cdata.oacs.doorbell_buffer_config = 1;
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	ctrlr.page_size = 0x1000;
	MOCK_CLEAR(spdk_malloc);
	MOCK_CLEAR(spdk_zmalloc);
	ret = nvme_ctrlr_set_doorbell_buffer_config(&ctrlr);
	CU_ASSERT(ret == 0);
	nvme_ctrlr_free_doorbell_buffer(&ctrlr);
}

static void
test_nvme_ctrlr_test_active_ns(void)
{
	uint32_t		nsid, minor;
	size_t			ns_id_count;
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.page_size = 0x1000;

	for (minor = 0; minor <= 2; minor++) {
		SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
		ctrlr.state = NVME_CTRLR_STATE_READY;

		ctrlr.vs.bits.mjr = 1;
		ctrlr.vs.bits.mnr = minor;
		ctrlr.vs.bits.ter = 0;
		ctrlr.num_ns = ctrlr.cdata.nn = 1531;
		nvme_ctrlr_identify_active_ns(&ctrlr);

		for (nsid = 1; nsid <= ctrlr.num_ns; nsid++) {
			CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, nsid) == true);
		}

		for (; nsid <= 1559; nsid++) {
			CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, nsid) == false);
		}

		for (nsid = 0; nsid < ctrlr.num_ns; nsid++) {
			ctrlr.active_ns_list[nsid] = 0;
		}
		CU_ASSERT(spdk_nvme_ctrlr_get_first_active_ns(&ctrlr) == 0);

		ctrlr.active_ns_list[0] = 1;
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1) == true);
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 2) == false);
		nsid = spdk_nvme_ctrlr_get_first_active_ns(&ctrlr);
		CU_ASSERT(nsid == 1);

		ctrlr.active_ns_list[1] = 3;
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1) == true);
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 2) == false);
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 3) == true);
		nsid = spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, nsid);
		CU_ASSERT(nsid == 3);
		nsid = spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, nsid);
		CU_ASSERT(nsid == 0);

		memset(ctrlr.active_ns_list, 0, ctrlr.num_ns);
		for (nsid = 0; nsid < ctrlr.num_ns; nsid++) {
			ctrlr.active_ns_list[nsid] = nsid + 1;
		}

		ns_id_count = 0;
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(&ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, nsid)) {
			CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, nsid) == true);
			ns_id_count++;
		}
		CU_ASSERT(ns_id_count == ctrlr.num_ns);

		nvme_ctrlr_destruct(&ctrlr);
	}
}

static void
test_nvme_ctrlr_test_active_ns_error_case(void)
{
	int rc;
	struct spdk_nvme_ctrlr	ctrlr = {.state = NVME_CTRLR_STATE_READY};

	ctrlr.page_size = 0x1000;
	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 2;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cdata.nn = 2;

	set_status_code = SPDK_NVME_SC_INVALID_FIELD;
	rc = nvme_ctrlr_identify_active_ns(&ctrlr);
	CU_ASSERT(rc == -ENXIO);
	set_status_code = SPDK_NVME_SC_SUCCESS;
}

static void
test_nvme_ctrlr_init_delay(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	/* Test that the initialization delay works correctly.  We only
	 * do the initialization delay on SSDs that require it, so
	 * set that quirk here.
	 */
	ctrlr.quirks = NVME_QUIRK_DELAY_BEFORE_INIT;
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
	ctrlr.state = NVME_CTRLR_STATE_INIT_DELAY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(ctrlr.sleep_timeout_tsc != 0);

	/* delay 1s, just return as sleep time isn't enough */
	spdk_delay_us(1 * spdk_get_ticks_hz());
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(ctrlr.sleep_timeout_tsc != 0);

	/* sleep timeout, start to initialize */
	spdk_delay_us(2 * spdk_get_ticks_hz());
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);

	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_RESET_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_spdk_nvme_ctrlr_set_trid(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {0};
	struct spdk_nvme_transport_id	new_trid = {{0}};

	CU_ASSERT(pthread_mutex_init(&ctrlr.ctrlr_lock, NULL) == 0);

	ctrlr.is_failed = false;
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	snprintf(ctrlr.trid.subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode1");
	snprintf(ctrlr.trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", "192.168.100.8");
	snprintf(ctrlr.trid.trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%s", "4420");
	CU_ASSERT(spdk_nvme_ctrlr_set_trid(&ctrlr, &new_trid) == -EPERM);

	ctrlr.is_failed = true;
	new_trid.trtype = SPDK_NVME_TRANSPORT_TCP;
	CU_ASSERT(spdk_nvme_ctrlr_set_trid(&ctrlr, &new_trid) == -EINVAL);
	CU_ASSERT(ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_RDMA);

	new_trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	snprintf(new_trid.subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode2");
	CU_ASSERT(spdk_nvme_ctrlr_set_trid(&ctrlr, &new_trid) == -EINVAL);
	CU_ASSERT(strncmp(ctrlr.trid.subnqn, "nqn.2016-06.io.spdk:cnode1", SPDK_NVMF_NQN_MAX_LEN) == 0);


	snprintf(new_trid.subnqn, SPDK_NVMF_NQN_MAX_LEN, "%s", "nqn.2016-06.io.spdk:cnode1");
	snprintf(new_trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN, "%s", "192.168.100.9");
	snprintf(new_trid.trsvcid, SPDK_NVMF_TRSVCID_MAX_LEN, "%s", "4421");
	CU_ASSERT(spdk_nvme_ctrlr_set_trid(&ctrlr, &new_trid) == 0);
	CU_ASSERT(strncmp(ctrlr.trid.traddr, "192.168.100.9", SPDK_NVMF_TRADDR_MAX_LEN) == 0);
	CU_ASSERT(strncmp(ctrlr.trid.trsvcid, "4421", SPDK_NVMF_TRSVCID_MAX_LEN) == 0);

	CU_ASSERT(pthread_mutex_destroy(&ctrlr.ctrlr_lock) == 0);
}

static void
test_nvme_ctrlr_init_set_nvmf_ioccsz(void)
{
	struct spdk_nvme_ctrlr_data cdata = {};
	DECLARE_AND_CONSTRUCT_CTRLR();
	/* equivalent of 4096 bytes */
	cdata.nvmf_specific.ioccsz = 260;
	cdata.nvmf_specific.icdoff = 1;
	g_cdata = &cdata;

	/* Check PCI trtype, */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_NUM_QUEUES);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);

	CU_ASSERT(ctrlr.ioccsz_bytes == 0);
	CU_ASSERT(ctrlr.icdoff == 0);

	nvme_ctrlr_destruct(&ctrlr);

	/* Check RDMA trtype, */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_RDMA;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_NUM_QUEUES);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);

	CU_ASSERT(ctrlr.ioccsz_bytes == 4096);
	CU_ASSERT(ctrlr.icdoff == 1);
	ctrlr.ioccsz_bytes = 0;
	ctrlr.icdoff = 0;

	nvme_ctrlr_destruct(&ctrlr);

	/* Check TCP trtype, */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_TCP;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_NUM_QUEUES);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);

	CU_ASSERT(ctrlr.ioccsz_bytes == 4096);
	CU_ASSERT(ctrlr.icdoff == 1);
	ctrlr.ioccsz_bytes = 0;
	ctrlr.icdoff = 0;

	nvme_ctrlr_destruct(&ctrlr);

	/* Check FC trtype, */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_FC;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_NUM_QUEUES);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);

	CU_ASSERT(ctrlr.ioccsz_bytes == 4096);
	CU_ASSERT(ctrlr.icdoff == 1);
	ctrlr.ioccsz_bytes = 0;
	ctrlr.icdoff = 0;

	nvme_ctrlr_destruct(&ctrlr);

	/* Check CUSTOM trtype, */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_CUSTOM;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_NUM_QUEUES);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);

	CU_ASSERT(ctrlr.ioccsz_bytes == 0);
	CU_ASSERT(ctrlr.icdoff == 0);

	nvme_ctrlr_destruct(&ctrlr);

	g_cdata = NULL;
}

static void
test_nvme_ctrlr_init_set_num_queues(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> SET_IDENTIFY_IOCS_SPECIFIC */
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_IOCS_SPECIFIC);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> SET_NUM_QUEUES */
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_NUM_QUEUES);

	ctrlr.opts.num_io_queues = 64;
	/* Num queues is zero-based. So, use 31 to get 32 queues */
	fake_cpl.cdw0 = 31 + (31 << 16);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> IDENTIFY_ACTIVE_NS */
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS);
	CU_ASSERT(ctrlr.opts.num_io_queues == 32);
	fake_cpl.cdw0 = 0;

	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_set_keep_alive_timeout(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);

	ctrlr.opts.keep_alive_timeout_ms = 60000;
	ctrlr.cdata.kas = 1;
	ctrlr.state = NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT;
	fake_cpl.cdw0 = 120000;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> SET_HOST_ID */
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_HOST_ID);
	CU_ASSERT(ctrlr.opts.keep_alive_timeout_ms == 120000);
	fake_cpl.cdw0 = 0;

	/* Target does not support Get Feature "Keep Alive Timer" */
	ctrlr.opts.keep_alive_timeout_ms = 60000;
	ctrlr.cdata.kas = 1;
	ctrlr.state = NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT;
	set_status_code = SPDK_NVME_SC_INVALID_FIELD;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> SET_HOST_ID */
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_HOST_ID);
	CU_ASSERT(ctrlr.opts.keep_alive_timeout_ms == 60000);
	set_status_code = SPDK_NVME_SC_SUCCESS;

	/* Target fails Get Feature "Keep Alive Timer" for another reason */
	ctrlr.opts.keep_alive_timeout_ms = 60000;
	ctrlr.cdata.kas = 1;
	ctrlr.state = NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT;
	set_status_code = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> ERROR */
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ERROR);
	set_status_code = SPDK_NVME_SC_SUCCESS;

	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_alloc_io_qpair_fail(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0;

	setup_qpairs(&ctrlr, 1);

	/* Modify the connect_qpair return code to inject a failure */
	g_connect_qpair_return_code = 1;

	/* Attempt to allocate a qpair, this should fail */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0);
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	/* Verify that the qpair is removed from the lists */
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&ctrlr.active_io_qpairs));

	g_connect_qpair_return_code = 0;
	cleanup_qpairs(&ctrlr);
}

static void
test_nvme_ctrlr_add_remove_process(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	void *devhandle = (void *)0xDEADBEEF;
	struct spdk_nvme_ctrlr_process *proc = NULL;
	int rc;

	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	TAILQ_INIT(&ctrlr.active_procs);

	rc = nvme_ctrlr_add_process(&ctrlr, devhandle);
	CU_ASSERT(rc == 0);
	proc = TAILQ_FIRST(&ctrlr.active_procs);
	SPDK_CU_ASSERT_FATAL(proc != NULL);
	CU_ASSERT(proc->is_primary == true);
	CU_ASSERT(proc->pid == getpid());
	CU_ASSERT(proc->devhandle == (void *)0xDEADBEEF);
	CU_ASSERT(proc->ref == 0);

	nvme_ctrlr_remove_process(&ctrlr, proc);
	CU_ASSERT(TAILQ_EMPTY(&ctrlr.active_procs));
}

static void *
gpa_to_vva(void *prv, uint64_t addr, uint64_t len)
{
	return (void *)(uintptr_t)addr;
}

static void
test_nvme_cmd_map_prps(void)
{
	struct spdk_nvme_cmd cmd = {};
	struct iovec iovs[33];
	uint64_t phy_addr, *prp;
	uint32_t len;
	void *buf, *prps;
	int i, ret;
	size_t mps = 4096;

	buf = spdk_zmalloc(132 * 1024, 4096, &phy_addr, 0, 0);
	CU_ASSERT(buf != NULL);
	prps = spdk_zmalloc(4096, 4096, &phy_addr, 0, 0);
	CU_ASSERT(prps != NULL);

	/* test case 1: 4KiB with PRP1 only */
	cmd.dptr.prp.prp1 = (uint64_t)(uintptr_t)buf;
	len = 4096;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)cmd.dptr.prp.prp1);
	CU_ASSERT(iovs[0].iov_len == len);

	/* test case 2: 4KiB with PRP1 and PRP2, 1KiB in first iov, and 3KiB in second iov */
	cmd.dptr.prp.prp1 = (uint64_t)(uintptr_t)buf + 1024 * 3;
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)buf + 4096;
	len = 4096;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 1, len, mps, gpa_to_vva);
	CU_ASSERT(ret == -ERANGE);
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)cmd.dptr.prp.prp1);
	CU_ASSERT(iovs[0].iov_len == 1024);
	CU_ASSERT(iovs[1].iov_base == (void *)(uintptr_t)cmd.dptr.prp.prp2);
	CU_ASSERT(iovs[1].iov_len == 1024 * 3);

	/* test case 3: 128KiB with PRP list, 1KiB in first iov, 3KiB in last iov */
	cmd.dptr.prp.prp1 = (uint64_t)(uintptr_t)buf + 1024 * 3;
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)prps;
	len = 128 * 1024;
	prp = prps;
	for (i = 1; i < 33; i++) {
		*prp = (uint64_t)(uintptr_t)buf + i * 4096;
		prp++;
	}
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 33);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)cmd.dptr.prp.prp1);
	CU_ASSERT(iovs[0].iov_len == 1024);
	for (i = 1; i < 32; i++) {
		CU_ASSERT(iovs[i].iov_base == (void *)((uintptr_t)buf + i * 4096));
		CU_ASSERT(iovs[i].iov_len == 4096);
	}
	CU_ASSERT(iovs[32].iov_base == (void *)((uintptr_t)buf + 32 * 4096));
	CU_ASSERT(iovs[32].iov_len == 1024 * 3);

	/* test case 4: 256KiB with PRP list, not enough iovs */
	cmd.dptr.prp.prp1 = (uint64_t)(uintptr_t)buf + 1024 * 3;
	cmd.dptr.prp.prp2 = (uint64_t)(uintptr_t)prps;
	len = 256 * 1024;
	ret = nvme_cmd_map_prps(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == -ERANGE);

	spdk_free(buf);
	spdk_free(prps);
}

static void
test_nvme_cmd_map_sgls(void)
{
	struct spdk_nvme_cmd cmd = {};
	struct iovec iovs[33];
	uint64_t phy_addr;
	uint32_t len;
	void *buf, *sgls;
	struct spdk_nvme_sgl_descriptor *sgl;
	int i, ret;
	size_t mps = 4096;

	buf = spdk_zmalloc(132 * 1024, 4096, &phy_addr, 0, 0);
	CU_ASSERT(buf != NULL);
	sgls = spdk_zmalloc(4096, 4096, &phy_addr, 0, 0);
	CU_ASSERT(sgls != NULL);

	/* test case 1: 8KiB with 1 data block */
	len = 8192;
	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	cmd.dptr.sgl1.unkeyed.length = len;
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)buf;

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 1);
	CU_ASSERT(iovs[0].iov_base == buf);
	CU_ASSERT(iovs[0].iov_len == 8192);

	/* test case 2: 8KiB with 2 data blocks and 1 last segment */
	sgl = (struct spdk_nvme_sgl_descriptor *)sgls;
	sgl[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[0].unkeyed.length = 2048;
	sgl[0].address = (uint64_t)(uintptr_t)buf;
	sgl[1].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[1].unkeyed.length = len - 2048;
	sgl[1].address = (uint64_t)(uintptr_t)buf + 16 * 1024;

	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	cmd.dptr.sgl1.unkeyed.length = 2 * sizeof(*sgl);
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)sgls;

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)buf);
	CU_ASSERT(iovs[0].iov_len == 2048);
	CU_ASSERT(iovs[1].iov_base == (void *)((uintptr_t)buf + 16 * 1024));
	CU_ASSERT(iovs[1].iov_len == len - 2048);

	/* test case 3: 8KiB with 1 segment, 1 last segment and 3 data blocks */
	sgl[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[0].unkeyed.length = 2048;
	sgl[0].address = (uint64_t)(uintptr_t)buf;
	sgl[1].unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	sgl[1].unkeyed.length = 2 * sizeof(*sgl);
	sgl[1].address = (uint64_t)(uintptr_t)&sgl[9];

	sgl[9].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[9].unkeyed.length = 4096;
	sgl[9].address = (uint64_t)(uintptr_t)buf + 4 * 1024;
	sgl[10].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	sgl[10].unkeyed.length = 2048;
	sgl[10].address = (uint64_t)(uintptr_t)buf + 16 * 1024;

	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_SEGMENT;
	cmd.dptr.sgl1.unkeyed.length = 2 * sizeof(*sgl);
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)&sgl[0];

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 33, len, mps, gpa_to_vva);
	CU_ASSERT(ret == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)(uintptr_t)buf);
	CU_ASSERT(iovs[0].iov_len == 2048);
	CU_ASSERT(iovs[1].iov_base == (void *)((uintptr_t)buf + 4 * 1024));
	CU_ASSERT(iovs[1].iov_len == 4096);
	CU_ASSERT(iovs[2].iov_base == (void *)((uintptr_t)buf + 16 * 1024));
	CU_ASSERT(iovs[2].iov_len == 2048);

	/* test case 4: not enough iovs */
	len = 12 * 1024;
	for (i = 0; i < 6; i++) {
		sgl[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl[0].unkeyed.length = 2048;
		sgl[0].address = (uint64_t)(uintptr_t)buf + i * 4096;
	}

	cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	cmd.dptr.sgl1.unkeyed.length = 6 * sizeof(*sgl);
	cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)sgls;

	ret = nvme_cmd_map_sgls(NULL, &cmd, iovs, 4, len, mps, gpa_to_vva);
	CU_ASSERT(ret == -ERANGE);

	spdk_free(buf);
	spdk_free(sgls);
}

static void
test_nvme_ctrlr_set_arbitration_feature(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};

	ctrlr.opts.arbitration_burst = 6;
	ctrlr.flags |= SPDK_NVME_CTRLR_WRR_SUPPORTED;
	ctrlr.opts.low_priority_weight = 1;
	ctrlr.opts.medium_priority_weight = 2;
	ctrlr.opts.high_priority_weight = 3;
	/* g_ut_cdw11 used to record value command feature set. */
	g_ut_cdw11 = 0;

	/* arbitration_burst count available. */
	nvme_ctrlr_set_arbitration_feature(&ctrlr);
	CU_ASSERT((uint8_t)g_ut_cdw11 == 6);
	CU_ASSERT((uint8_t)(g_ut_cdw11 >> 8) == 1);
	CU_ASSERT((uint8_t)(g_ut_cdw11 >> 16) == 2);
	CU_ASSERT((uint8_t)(g_ut_cdw11 >> 24) == 3);

	/* arbitration_burst unavailable. */
	g_ut_cdw11 = 0;
	ctrlr.opts.arbitration_burst = 8;

	nvme_ctrlr_set_arbitration_feature(&ctrlr);
	CU_ASSERT(g_ut_cdw11 == 0);
}

static void
test_nvme_ctrlr_set_state(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	MOCK_SET(spdk_get_ticks, 0);

	nvme_ctrlr_set_state(&ctrlr, NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT, 1000);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT);
	CU_ASSERT(ctrlr.state_timeout_tsc == 1000000);

	nvme_ctrlr_set_state(&ctrlr, NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT, 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT);
	CU_ASSERT(ctrlr.state_timeout_tsc == NVME_TIMEOUT_INFINITE);

	/* Time out ticks causes integer overflow. */
	MOCK_SET(spdk_get_ticks, UINT64_MAX);

	nvme_ctrlr_set_state(&ctrlr, NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT, 1000);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_SET_KEEP_ALIVE_TIMEOUT);
	CU_ASSERT(ctrlr.state_timeout_tsc == NVME_TIMEOUT_INFINITE);
	MOCK_CLEAR(spdk_get_ticks);
}

static void
test_nvme_ctrlr_active_ns_list_v0(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 0;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cdata.nn = 1024;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> CONSTRUCT_NS */
	SPDK_CU_ASSERT_FATAL(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1));
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1024));
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1025));
	CU_ASSERT(spdk_nvme_ctrlr_get_first_active_ns(&ctrlr) == 1);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1023) == 1024);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1024) == 0);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1025) == 0);

	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_active_ns_list_v2(void)
{
	uint32_t i;
	uint32_t active_ns_list[1024];
	DECLARE_AND_CONSTRUCT_CTRLR();

	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 2;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cdata.nn = 4096;

	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);

	/* No active namespaces */
	memset(active_ns_list, 0, sizeof(active_ns_list));
	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> CONSTRUCT_NS */
	SPDK_CU_ASSERT_FATAL(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1));
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1024));
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1025));
	CU_ASSERT(spdk_nvme_ctrlr_get_first_active_ns(&ctrlr) == 0);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1024) == 0);

	nvme_ctrlr_destruct(&ctrlr);

	/* 1024 active namespaces - one full page */
	memset(active_ns_list, 0, sizeof(active_ns_list));
	for (i = 0; i < 1024; ++i) {
		active_ns_list[i] = i + 1;
	}

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> CONSTRUCT_NS */
	SPDK_CU_ASSERT_FATAL(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1));
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1024));
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1025));
	CU_ASSERT(spdk_nvme_ctrlr_get_first_active_ns(&ctrlr) == 1);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1023) == 1024);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1024) == 0);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1025) == 0);

	nvme_ctrlr_destruct(&ctrlr);

	/* 1023 active namespaces - full page minus one	 */
	memset(active_ns_list, 0, sizeof(active_ns_list));
	for (i = 0; i < 1023; ++i) {
		active_ns_list[i] = i + 1;
	}

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0); /* -> CONSTRUCT_NS */
	SPDK_CU_ASSERT_FATAL(ctrlr.state == NVME_CTRLR_STATE_CONSTRUCT_NS);
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1));
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1023));
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1024));
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1025));
	CU_ASSERT(spdk_nvme_ctrlr_get_first_active_ns(&ctrlr) == 1);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1023) == 0);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1024) == 0);
	CU_ASSERT(spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, 1025) == 0);

	nvme_ctrlr_destruct(&ctrlr);

	g_active_ns_list = NULL;
	g_active_ns_list_length = 0;
}

static void
test_nvme_ctrlr_ns_mgmt(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	uint32_t active_ns_list[] = { 1, 2, 100, 1024 };
	uint32_t active_ns_list2[] = { 1, 2, 3, 100, 1024 };
	struct spdk_nvme_ns_data nsdata = {};
	struct spdk_nvme_ctrlr_list ctrlr_list = {};
	uint32_t nsid;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);

	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 2;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cdata.nn = 4096;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0);
	}

	fake_cpl.cdw0 = 3;
	nsid = spdk_nvme_ctrlr_create_ns(&ctrlr, &nsdata);
	fake_cpl.cdw0 = 0;
	CU_ASSERT(nsid == 3);
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 3));
	CU_ASSERT(spdk_nvme_ctrlr_get_ns(&ctrlr, 3) != NULL);

	g_active_ns_list = active_ns_list2;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list2);
	CU_ASSERT(spdk_nvme_ctrlr_attach_ns(&ctrlr, 3, &ctrlr_list) == 0);
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 3));
	CU_ASSERT(spdk_nvme_ctrlr_get_ns(&ctrlr, 3) != NULL);

	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);
	CU_ASSERT(spdk_nvme_ctrlr_detach_ns(&ctrlr, 3, &ctrlr_list) == 0);
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 3));
	CU_ASSERT(spdk_nvme_ctrlr_get_ns(&ctrlr, 3) != NULL);

	CU_ASSERT(spdk_nvme_ctrlr_delete_ns(&ctrlr, 3) == 0);
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 3));
	CU_ASSERT(spdk_nvme_ctrlr_get_ns(&ctrlr, 3) != NULL);
	g_active_ns_list = 0;
	g_active_ns_list_length = 0;

	nvme_ctrlr_destruct(&ctrlr);
}

static void check_en_set_rdy(void)
{
	if (g_ut_nvme_regs.cc.bits.en == 1) {
		g_ut_nvme_regs.csts.bits.rdy = 1;
	}
}

static void
test_nvme_ctrlr_reset(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	struct spdk_nvme_ctrlr_data cdata = { .nn = 4096 };
	uint32_t active_ns_list[] = { 1, 2, 100, 1024 };
	uint32_t active_ns_list2[] = { 1, 100, 1024 };

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);

	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 2;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cdata.nn = 2048;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0);
	}
	CU_ASSERT(spdk_nvme_ctrlr_get_num_ns(&ctrlr) == 2048);
	CU_ASSERT(spdk_nvme_ctrlr_get_ns(&ctrlr, 2) != NULL);
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 2));

	/* Reset controller with changed number of namespaces */
	g_cdata = &cdata;
	g_active_ns_list = active_ns_list2;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list2);
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);
	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));
	g_set_reg_cb = check_en_set_rdy;
	CU_ASSERT(spdk_nvme_ctrlr_reset(&ctrlr) == 0);
	g_set_reg_cb = NULL;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);
	g_cdata = NULL;
	g_active_ns_list = 0;
	g_active_ns_list_length = 0;

	CU_ASSERT(spdk_nvme_ctrlr_get_num_ns(&ctrlr) == 4096);
	CU_ASSERT(spdk_nvme_ctrlr_get_ns(&ctrlr, 2) != NULL);
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 2));

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static uint32_t g_aer_cb_counter;

static void
aer_cb(void *aer_cb_arg, const struct spdk_nvme_cpl *cpl)
{
	g_aer_cb_counter++;
}

static void
test_nvme_ctrlr_aer_callback(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	uint32_t active_ns_list[] = { 1, 2, 100, 1024 };
	union spdk_nvme_async_event_completion	aer_event = {
		.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE,
		.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED
	};
	struct spdk_nvme_cpl aer_cpl = {
		.status.sct = SPDK_NVME_SCT_GENERIC,
		.status.sc = SPDK_NVME_SC_SUCCESS,
		.cdw0 = aer_event.raw
	};

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);

	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 2;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cdata.nn = 4096;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0);
	}

	CU_ASSERT(nvme_ctrlr_add_process(&ctrlr, NULL) == 0);
	spdk_nvme_ctrlr_register_aer_callback(&ctrlr, aer_cb, NULL);

	/* Async event */
	g_aer_cb_counter = 0;
	nvme_ctrlr_async_event_cb(&ctrlr.aer[0], &aer_cpl);
	nvme_ctrlr_complete_queued_async_events(&ctrlr);
	CU_ASSERT(g_aer_cb_counter == 1);
	g_active_ns_list = 0;
	g_active_ns_list_length = 0;

	nvme_ctrlr_free_processes(&ctrlr);
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_ns_attr_changed(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();
	uint32_t active_ns_list[] = { 1, 2, 100, 1024 };
	uint32_t active_ns_list2[] = { 1, 2, 1024 };
	uint32_t active_ns_list3[] = { 1, 2, 101, 1024 };
	union spdk_nvme_async_event_completion	aer_event = {
		.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE,
		.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED
	};
	struct spdk_nvme_cpl aer_cpl = {
		.status.sct = SPDK_NVME_SCT_GENERIC,
		.status.sc = SPDK_NVME_SC_SUCCESS,
		.cdw0 = aer_event.raw
	};

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);

	ctrlr.vs.bits.mjr = 1;
	ctrlr.vs.bits.mnr = 3;
	ctrlr.vs.bits.ter = 0;
	ctrlr.cap.bits.css |= SPDK_NVME_CAP_CSS_IOCS;
	ctrlr.cdata.nn = 4096;

	ctrlr.state = NVME_CTRLR_STATE_IDENTIFY_ACTIVE_NS;
	g_active_ns_list = active_ns_list;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list);
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		SPDK_CU_ASSERT_FATAL(nvme_ctrlr_process_init(&ctrlr) == 0);
	}

	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 100));

	CU_ASSERT(nvme_ctrlr_add_process(&ctrlr, NULL) == 0);
	spdk_nvme_ctrlr_register_aer_callback(&ctrlr, aer_cb, NULL);

	/* Remove NS 100 */
	g_aer_cb_counter = 0;
	g_active_ns_list = active_ns_list2;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list2);
	nvme_ctrlr_async_event_cb(&ctrlr.aer[0], &aer_cpl);
	nvme_ctrlr_complete_queued_async_events(&ctrlr);
	CU_ASSERT(g_aer_cb_counter == 1);
	CU_ASSERT(!spdk_nvme_ctrlr_is_active_ns(&ctrlr, 100));

	/* Add NS 101 */
	g_active_ns_list = active_ns_list3;
	g_active_ns_list_length = SPDK_COUNTOF(active_ns_list3);
	nvme_ctrlr_async_event_cb(&ctrlr.aer[0], &aer_cpl);
	nvme_ctrlr_complete_queued_async_events(&ctrlr);
	CU_ASSERT(g_aer_cb_counter == 2);
	CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 101));

	g_active_ns_list = 0;
	g_active_ns_list_length = 0;
	nvme_ctrlr_free_processes(&ctrlr);
	nvme_ctrlr_destruct(&ctrlr);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_ctrlr", NULL, NULL);

	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_1_rdy_0);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_1_rdy_1);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_0_rdy_0);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_0_rdy_1);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_0_rdy_0_ams_rr);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_0_rdy_0_ams_wrr);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_en_0_rdy_0_ams_vs);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_delay);
	CU_ADD_TEST(suite, test_alloc_io_qpair_rr_1);
	CU_ADD_TEST(suite, test_ctrlr_get_default_ctrlr_opts);
	CU_ADD_TEST(suite, test_ctrlr_get_default_io_qpair_opts);
	CU_ADD_TEST(suite, test_alloc_io_qpair_wrr_1);
	CU_ADD_TEST(suite, test_alloc_io_qpair_wrr_2);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_update_firmware);
	CU_ADD_TEST(suite, test_nvme_ctrlr_fail);
	CU_ADD_TEST(suite, test_nvme_ctrlr_construct_intel_support_log_page_list);
	CU_ADD_TEST(suite, test_nvme_ctrlr_set_supported_features);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_doorbell_buffer_config);
#if 0 /* TODO: move to PCIe-specific unit test */
	CU_ADD_TEST(suite, test_nvme_ctrlr_alloc_cmb);
#endif
	CU_ADD_TEST(suite, test_nvme_ctrlr_test_active_ns);
	CU_ADD_TEST(suite, test_nvme_ctrlr_test_active_ns_error_case);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_reconnect_io_qpair);
	CU_ADD_TEST(suite, test_spdk_nvme_ctrlr_set_trid);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_set_nvmf_ioccsz);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_set_num_queues);
	CU_ADD_TEST(suite, test_nvme_ctrlr_init_set_keep_alive_timeout);
	CU_ADD_TEST(suite, test_alloc_io_qpair_fail);
	CU_ADD_TEST(suite, test_nvme_ctrlr_add_remove_process);
	CU_ADD_TEST(suite, test_nvme_cmd_map_prps);
	CU_ADD_TEST(suite, test_nvme_cmd_map_sgls);
	CU_ADD_TEST(suite, test_nvme_ctrlr_set_arbitration_feature);
	CU_ADD_TEST(suite, test_nvme_ctrlr_set_state);
	CU_ADD_TEST(suite, test_nvme_ctrlr_active_ns_list_v0);
	CU_ADD_TEST(suite, test_nvme_ctrlr_active_ns_list_v2);
	CU_ADD_TEST(suite, test_nvme_ctrlr_ns_mgmt);
	CU_ADD_TEST(suite, test_nvme_ctrlr_reset);
	CU_ADD_TEST(suite, test_nvme_ctrlr_aer_callback);
	CU_ADD_TEST(suite, test_nvme_ctrlr_ns_attr_changed);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}