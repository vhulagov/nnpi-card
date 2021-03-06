/********************************************
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 ********************************************/

#include "sphcs_inf.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "sphcs_cs.h"
#include "ioctl_inf.h"
#include "sph_log.h"
#include "sph_error.h"
#include "inf_cmdq.h"
#include "inf_context.h"
#include "inf_devres.h"
#include "inf_devnet.h"
#include "inf_copy.h"
#include "inf_cpylst.h"
#include "inf_subresload.h"
#include "inf_req.h"
#include "sph_boot_defs.h"
#include "sphcs_trace.h"
#include "sphcs_ctx_uids.h"
#include "sphcs_cmd_chan.h"
#include "safe_mem_lib.h"
#include <linux/module.h>

/* min system memory threshold in KB */
static uint32_t mem_thr;
module_param(mem_thr, uint, 0644);

static inline uint32_t getFreeMem(void)
{
	struct sysinfo i;

	si_meminfo(&i);
	return  i.freeram * (i.mem_unit >> 10);
}

static inline bool check_memory_threshold(void)
{
	uint32_t freeRam;

	if (mem_thr) {
		freeRam = getFreeMem();
		if (freeRam < mem_thr) {
			sph_log_info(CREATE_COMMAND_LOG, "free memory below the threshold (%u) freeRam (%u)", mem_thr, freeRam);
			return false;
		}
	}
	return true;
}

static struct cdev s_cdev;
static dev_t       s_devnum;
static struct class *s_class;
static struct device *s_dev;

struct inf_daemon {
	struct inf_cmd_queue cmdq;
	struct list_head alloc_req_list;
	spinlock_t       lock;
};

struct alloc_req {
	sphcs_alloc_resource_callback cb;
	void                         *context;
	struct list_head              node;
};

struct req_params {
	uint16_t             idx;
	size_t               size;
	//priority 0 == normal, 1 == high
	uint8_t              priority;

	union {
		struct {
			uint16_t           cpy_idx;
		};//cpylst
		//copy does not has specific params
		struct {
			bool              sched_params_is_null;
			uint8_t           debugOn : 1;
			uint8_t           collectInfo : 1;
			uint8_t           reserved : 6;
		};//infreq
	};
};

static int init_daemon(struct inf_data *inf_data)
{
	mutex_lock(&inf_data->io_lock);

	if (unlikely(inf_data->daemon != NULL)) {
		mutex_unlock(&inf_data->io_lock);
		return -EEXIST;
	}

	inf_data->daemon = kzalloc(sizeof(*inf_data->daemon), GFP_NOWAIT);
	if (unlikely(inf_data->daemon == NULL)) {
		mutex_unlock(&inf_data->io_lock);
		return -ENOMEM;
	}

	inf_cmd_queue_init(&inf_data->daemon->cmdq);

	INIT_LIST_HEAD(&inf_data->daemon->alloc_req_list);
	spin_lock_init(&inf_data->daemon->lock);

	// Set card boot state as "Card ready"
	g_the_sphcs->hw_ops->set_card_doorbell_value(g_the_sphcs->hw_handle,
				    (SPH_CARD_BOOT_STATE_CARD_READY <<
				     SPH_CARD_BOOT_STATE_SHIFT));

	mutex_unlock(&inf_data->io_lock);
	return 0;
}

#ifdef ULT
static int switch_daemon(struct inf_data *inf_data)
{
	int ret;

	mutex_lock(&inf_data->io_lock);

	if (unlikely(inf_data->ult_daemon_save != NULL)) {
		mutex_unlock(&inf_data->io_lock);
		return -EEXIST;
	}

	if (likely(inf_data->daemon != NULL)) {
		inf_data->ult_daemon_save = inf_data->daemon;
		inf_data->daemon = NULL;
	}

	mutex_unlock(&inf_data->io_lock);

	ret = init_daemon(inf_data);

	if (unlikely(ret < 0)) {
		mutex_lock(&inf_data->io_lock);
		inf_data->daemon = inf_data->ult_daemon_save;
		inf_data->ult_daemon_save = NULL;
		mutex_unlock(&inf_data->io_lock);
	}

	return ret;
}
#endif

static void release_pending_create_context_reuquests(void *cmd_args);

static void fini_daemon(struct inf_data *inf_data)
{
	struct alloc_req *req;
	struct inf_context *context;
	bool ctx_found = true;
	int i;

	mutex_lock(&inf_data->io_lock);

	/*
	 * if pending allocation requests have not been replied
	 * use the request callback to report error to the requester.
	 */
	SPH_SPIN_LOCK(&inf_data->daemon->lock);
	while (!list_empty(&inf_data->daemon->alloc_req_list)) {
		req = list_first_entry(&inf_data->daemon->alloc_req_list, struct alloc_req, node);
		list_del(&req->node);
		SPH_SPIN_UNLOCK(&inf_data->daemon->lock);
		req->cb(g_the_sphcs,
			req->context,
			-1,
			IOCTL_SPHCS_NO_DEVICE);
		kfree(req);
		SPH_SPIN_LOCK(&inf_data->daemon->lock);
	}
	SPH_SPIN_UNLOCK(&inf_data->daemon->lock);

	sph_log_debug(START_UP_LOG, "Send context failed to all pending requests\n");
	//Delete all pending context request and send failed message to host
	inf_cmd_queue_exe(&inf_data->daemon->cmdq, SPHCS_DAEMON_CMD_CREATE_CONTEXT, release_pending_create_context_reuquests);

	/* Pass through all existing context and release daemon reference
	 * since context could be removed from hash in inf_context_put,
	 * so we need to pass one by one on the hash and release reference
	 */
	do {
		ctx_found = false;
		SPH_SPIN_LOCK_BH(&inf_data->lock_bh);
		hash_for_each(inf_data->context_hash, i, context, hash_node) {
			/* check if context daemon reference didn't released yet, then do it.
			 * this needed in case some runtime died, then daemon got killed
			 */
			if (!context->daemon_ref_released) {
				ctx_found = true;
				SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);
				context->daemon_ref_released = true;
				inf_context_put(context);
				break;
			}
		}
	} while (ctx_found);
	SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);

	inf_cmd_queue_fini(&inf_data->daemon->cmdq);

	kfree(inf_data->daemon);
#ifdef ULT
	inf_data->daemon = inf_data->ult_daemon_save;
	inf_data->ult_daemon_save = NULL;
#else
	inf_data->daemon = NULL;
#endif

	// Set card boot state as "Driver ready"
	if (inf_data->daemon == NULL)
		g_the_sphcs->hw_ops->set_card_doorbell_value(g_the_sphcs->hw_handle,
							     (SPH_CARD_BOOT_STATE_DRV_READY <<
							      SPH_CARD_BOOT_STATE_SHIFT));

	mutex_unlock(&inf_data->io_lock);
}

static struct inf_context *find_and_get_context(struct inf_data *inf_data, uint16_t protocolID)
{
	struct inf_context *context;

	SPH_SPIN_LOCK_BH(&inf_data->lock_bh);
	hash_for_each_possible(inf_data->context_hash,
			       context,
			       hash_node,
			       protocolID)
		if (context->protocolID == protocolID) {
			if (unlikely(inf_context_get(context) == 0))
				break; //release is in progress
			SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);
			return context;
		}
	SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);

	return NULL;
}

static void recover_context(struct sphcs *sphcs, struct inf_context *context)
{
	uint8_t event = SPH_IPC_RECOVER_CONTEXT_SUCCESS;
	enum event_val val = 0;

	SPH_ASSERT(context != NULL);
	sph_log_info(CREATE_COMMAND_LOG, "Attempt to recover context (0x%x) from state: %u\n",
		     context->protocolID, inf_context_get_state(context));

	switch (inf_context_get_state(context)) {
	case CONTEXT_BROKEN_RECOVERABLE:
		inf_context_set_state(context, CONTEXT_OK);
		break;
	case CONTEXT_BROKEN_NON_RECOVERABLE:
		sph_log_info(CREATE_COMMAND_LOG, "Unable to recover context (0x%x) from non recoverable state\n",
				context->protocolID);
		event = SPH_IPC_RECOVER_CONTEXT_FAILED;
		val = SPH_IPC_CONTEXT_BROKEN;
		break;
	case CONTEXT_OK:
		sph_log_info(CREATE_COMMAND_LOG, "Got request to recover non-broken context: 0x%x\n",
				context->protocolID);
		break;
	default:
		sph_log_info(CREATE_COMMAND_LOG, "Unable to recover context (0x%x) which is in unknown state %d\n",
				context->protocolID, context->state);
		event = SPH_IPC_RECOVER_CONTEXT_FAILED;
		val = SPH_IPC_CONTEXT_BROKEN;
	}
	sphcs_send_event_report(sphcs, event, val, context->protocolID, -1);
}

static inline void destroy_context_on_create_failed(struct sphcs *sphcs, struct inf_context *context)
{
	SPH_SPIN_LOCK_BH(&sphcs->inf_data->lock_bh);
	SPH_ASSERT(context->attached == 0);
	if (unlikely(context->destroyed)) {
		SPH_SPIN_UNLOCK_BH(&sphcs->inf_data->lock_bh);
		return;
	}
	context->destroyed = -1;
	SPH_SPIN_UNLOCK_BH(&sphcs->inf_data->lock_bh);

	inf_context_put(context);
}

static inline int find_and_destroy_context(struct inf_data *inf_data, uint16_t ctxID)
{
	struct inf_context *iter, *context = NULL;

	SPH_SPIN_LOCK_BH(&inf_data->lock_bh);
	hash_for_each_possible(inf_data->context_hash, iter, hash_node, ctxID)
		if (iter->protocolID == ctxID) {
			context = iter;
			break;
		}

	if (unlikely(context == NULL)) {
		SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);
		return -ENXIO;
	}

	if (context->destroyed) {
		SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);
		return 0;
	}

	context->destroyed = 1;
	SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);

	inf_context_put(context);

	return 0;
}

static void sphcs_inf_context_chan_cleanup(struct sphcs_cmd_chan *chan, void *cb_ctx)
{
	struct inf_context *context = (struct inf_context *)cb_ctx;

	inf_context_destroy_objects(context);
	find_and_destroy_context(g_the_sphcs->inf_data, context->protocolID);
}

enum event_val create_context(struct sphcs *sphcs, uint16_t protocolID, uint8_t flags, uint32_t uid, struct sphcs_cmd_chan *chan)
{
	struct inf_context *context;
	struct inf_create_context cmd_args;
	int ret;

	ret = inf_context_create(protocolID, chan, &context);
	if (unlikely(ret < 0))
		return SPH_IPC_NO_MEMORY;

	CTX_UIDS_SET_UID(context->protocolID, uid);

	/* place a create context command for the daemon */
	cmd_args.contextID = protocolID;
	cmd_args.flags = flags;

	mutex_lock(&sphcs->inf_data->io_lock);
	if (unlikely(!sphcs->inf_data->daemon)) {
		mutex_unlock(&sphcs->inf_data->io_lock);
		destroy_context_on_create_failed(sphcs, context);
		return SPH_IPC_NO_DAEMON;
	}
	// Take kref, dedicated to daemon
	inf_context_get(context);
	context->daemon_ref_released = false;

	ret = inf_cmd_queue_add(&sphcs->inf_data->daemon->cmdq,
				SPHCS_DAEMON_CMD_CREATE_CONTEXT,
				&cmd_args,
				sizeof(cmd_args),
				NULL, NULL);

	if (unlikely(ret < 0)) {
		destroy_context_on_create_failed(sphcs, context);
		// release kref dedicated for daemon
		inf_context_put(context);
		mutex_unlock(&sphcs->inf_data->io_lock);
		return SPH_IPC_NO_MEMORY;
	}
	mutex_unlock(&sphcs->inf_data->io_lock);

	if (chan != NULL) {
		chan->destroy_cb = sphcs_inf_context_chan_cleanup;
		chan->destroy_cb_ctx = context;
	}

	return 0;
}

static void detach_runtime(struct sphcs *sphcs, struct inf_context *context)
{
	if (likely(context->attached > 0)) {

		context->attached = -1; //Lock is not needed here

		if (unlikely(!context->destroyed)) {
			inf_context_set_state(context, CONTEXT_BROKEN_NON_RECOVERABLE);
			sphcs_send_event_report(sphcs,
						SPH_IPC_ERROR_RUNTIME_DIED,
						0,
						context->protocolID,
						-1);
		}

		del_all_active_create_and_inf_requests(context);
		// no runtime attached anymore, release kref took for runtime
		inf_context_put(context);
	}
}

void handle_daemon_error(const struct inf_error_ioctl *err_ioctl)
{
	struct inf_context *context;

	if (err_ioctl->errorCode == SPH_IPC_ERROR_RUNTIME_LAUNCH ||
	    err_ioctl->errorCode == SPH_IPC_ERROR_RUNTIME_DIED ||
	    err_ioctl->errorCode == SPH_IPC_RUNTIME_DONE) {

		if (err_ioctl->errorCode != SPH_IPC_RUNTIME_DONE)
			sph_log_err(GENERAL_LOG, "got daemon error %d val=%d\n",
					err_ioctl->errorCode, err_ioctl->errorVal);

		context = find_and_get_context(g_the_sphcs->inf_data,
							   err_ioctl->errorVal);
		if (unlikely(context == NULL)) {
			sph_log_err(GENERAL_LOG, "Got error(%u) for not existing context(%u)\n",
					err_ioctl->errorCode, err_ioctl->errorVal);
			return;
		}
		SPH_ASSERT(!context->daemon_ref_released);
		inf_context_put(context);

		/* if the failure happened during context creation phase
		 * (have not yet attached or detached) destroy the context
		 */
		if (context->attached == 0) {

			enum event_val val;

			if (err_ioctl->errorCode == SPH_IPC_ERROR_RUNTIME_LAUNCH) {
				val = SPH_IPC_RUNTIME_LAUNCH_FAILED;
			} else {
				SPH_ASSERT(err_ioctl->errorCode == SPH_IPC_ERROR_RUNTIME_DIED);
				val = SPH_IPC_RUNTIME_FAILED;
			}

			sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_CREATE_CONTEXT_FAILED,
					val,
					err_ioctl->errorVal,
					-1);
			destroy_context_on_create_failed(g_the_sphcs, context);
		}
		context->daemon_ref_released = true;
		inf_context_put(context);
	} else {
		sph_log_err(GENERAL_LOG, "Got unknown error code from daemon %u\n",
			    err_ioctl->errorCode);
	}
}

void handle_runtime_error(struct inf_context *context,
			  const struct inf_error_ioctl *err_ioctl)
{
}

/*****************************************************************************
 * Inference cdev file operations (interface to daemon/runtime)
 *****************************************************************************/
static inline int is_inf_file(struct file *f);

static int sphcs_inf_open(struct inode *inode, struct file *f)
{
	if (unlikely(!is_inf_file(f)))
		return -EINVAL;

	f->private_data = NULL;

	return 0;
}

static int sphcs_inf_release(struct inode *inode, struct file *f)
{
	if (unlikely(!is_inf_file(f)))
		return -EINVAL;

	if (f->private_data == NULL)
		return 0;

	if (f->private_data == g_the_sphcs->inf_data->daemon) {
		fini_daemon(g_the_sphcs->inf_data);
	} else if (is_inf_context_ptr(f->private_data)) {
		struct inf_context *context = (struct inf_context *)f->private_data;

		detach_runtime(g_the_sphcs, context);
	}

	return 0;
}

static int handle_get_alloc_pgt(void __user *arg)
{
	struct inf_get_alloc_pgt req;
	struct dma_buf   *dma_buf;
	struct dma_buf_attachment *dma_att;
	struct sg_table  *dma_map;
	struct scatterlist *sgl;
	struct inf_alloc_pgt_entry entry;
	uint32_t nchunks = 0;
	int ret = 0;

	ret = copy_from_user(&req, arg, sizeof(req));
	if (ret)
		return -EIO;

	if (req.num_entries < 1)
		return -EINVAL;

	dma_buf = dma_buf_get(req.buf_fd);
	if (PTR_ERR_OR_ZERO(dma_buf))
		return -EINVAL;

	dma_att = dma_buf_attach(dma_buf, g_the_sphcs->hw_device);
	if (PTR_ERR_OR_ZERO(dma_att)) {
		ret = -EINVAL;
		goto fail_attach;
	}

	dma_map = dma_buf_map_attachment(dma_att, DMA_BIDIRECTIONAL);
	if (PTR_ERR_OR_ZERO(dma_map)) {
		ret = -EINVAL;
		goto fail_map;
	}

	sgl = dma_map->sgl;
	while (sgl &&
	       nchunks < req.num_entries) {
		entry.phys = sgl->dma_address;
		entry.size = sgl->length;
		ret = copy_to_user(&req.entries[nchunks], &entry, sizeof(entry));
		if (ret) {
			ret = -EIO;
			goto done;
		}

		nchunks++;
		sgl = sg_next(sgl);
	}

	req.num_entries = nchunks;
	ret = copy_to_user(arg, &req, sizeof(req));
	if (ret)
		ret = -EIO;

done:
	dma_buf_unmap_attachment(dma_att,
				 dma_map,
				 DMA_BIDIRECTIONAL);
fail_map:
	dma_buf_detach(dma_buf, dma_att);
fail_attach:
	dma_buf_put(dma_buf);

	return ret;
}

static long sphcs_inf_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	long ret;
	uint32_t            contextID;
	struct inf_context *context;

	if (unlikely(!is_inf_file(f)))
		return -EINVAL;

	switch (cmd) {
	case IOCTL_INF_ATTACH_DAEMON:
		ret = init_daemon(g_the_sphcs->inf_data);
		if (unlikely(ret < 0))
			return ret;

		f->private_data = g_the_sphcs->inf_data->daemon;
		break;
#ifdef ULT
	case IOCTL_INF_SWITCH_DAEMON:
		ret = switch_daemon(g_the_sphcs->inf_data);
		if (unlikely(ret < 0))
			return ret;

		f->private_data = g_the_sphcs->inf_data->daemon;
		break;
#endif
	case IOCTL_INF_ALLOC_RESOURCE_REPLY: {
		struct inf_alloc_resource_reply reply;
		struct alloc_req *req;

		ret = copy_from_user(&reply,
				     (void __user *)arg,
				     sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		if (unlikely(f->private_data != g_the_sphcs->inf_data->daemon))
			return -EINVAL;

		SPH_SPIN_LOCK(&g_the_sphcs->inf_data->daemon->lock);
		list_for_each_entry(req,
				    &g_the_sphcs->inf_data->daemon->alloc_req_list,
				    node) {
			if ((uint64_t)req == reply.drv_handle) {
				list_del(&req->node);
				break;
			}
		}
		SPH_SPIN_UNLOCK(&g_the_sphcs->inf_data->daemon->lock);
		if (unlikely(&req->node == &g_the_sphcs->inf_data->daemon->alloc_req_list))
			return -EINVAL;

		req->cb(g_the_sphcs,
			req->context,
			reply.buf_fd,
			reply.i_sphcs_err);
		kfree(req);
		break;
	}

	case IOCTL_INF_ATTACH_CONTEXT:
		if (unlikely(f->private_data != NULL))
			return -EBUSY;

		ret = copy_from_user(&contextID,
				     (void __user *)arg,
				     sizeof(uint32_t));
		if (unlikely(ret != 0))
			return -EIO;

		context = find_and_get_context(g_the_sphcs->inf_data, contextID);
		if (unlikely(context == NULL))
			return -ENXIO;

		ret = inf_context_runtime_attach(context);
		if (unlikely(ret < 0)) {
			if (ret == -EPERM) {
				SPH_ASSERT(context->destroyed == 1);//This can happen only if user canceled create
				destroy_context_on_create_failed(g_the_sphcs, context);
			} else {
				sph_log_debug(GENERAL_LOG, "Context %u was tried to be attached more than once\n", contextID);
			}
			inf_context_put(context);
			return ret;
		}
		inf_context_put(context);

		f->private_data = context;

		sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_CREATE_CONTEXT_SUCCESS,
					0,
					contextID,
					-1);

		sph_log_debug(GENERAL_LOG, "Context %u attached\n", contextID);

		DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_CONTEXT, contextID, contextID, SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));
		break;
	case IOCTL_INF_RESOURCE_CREATE_REPLY: {
		struct inf_create_resource_reply reply;
		struct inf_devres *devres;
		enum event_val eventVal;
		u16 event_val;
		int obj_id_1;
		int obj_id_2;

		ret = copy_from_user(&reply,
				     (void __user *)arg,
				     sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		devres = (struct inf_devres *)(uintptr_t)reply.drv_handle;
		if (unlikely(!is_inf_devres_ptr(devres)))
			return -EINVAL;

		if (unlikely(reply.i_sphcs_err != IOCTL_SPHCS_NO_ERROR)) {
			switch (reply.i_sphcs_err) {
			case IOCTL_SPHCS_ALLOC_FAILED:
			case IOCTL_SPHCS_NO_MEMORY:
				eventVal = SPH_IPC_NO_MEMORY;
				break;
			default:
				eventVal = SPH_IPC_RUNTIME_FAILED;
			}
			sph_log_err(CREATE_COMMAND_LOG, "runtime create_devres failed. err:%u.", reply.i_sphcs_err);
			goto failed_devres;
		}

		devres->rt_handle = reply.rt_handle;
		ret = inf_devres_attach_buf(devres,
					    reply.buf_fd);
		if (unlikely(ret < 0)) {
			eventVal = SPH_IPC_NO_MEMORY;
			sph_log_err(CREATE_COMMAND_LOG, "inf_devres_attach_buf failed. err:%ld.", ret);
			goto send_rt_devres_destr;
		}

		DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_DEVRES, devres->context->protocolID,
			devres->protocolID, SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));

		/* Register buffer with p2p infrastructure */
		if (inf_devres_is_p2p(devres))
			inf_devres_add_to_p2p(devres);

		/* Send event */
		event_val = 0;
		obj_id_1 = devres->protocolID;
		obj_id_2 = -1;

		if (devres->is_p2p_dst)
			obj_id_2 = (sg_dma_address(&devres->dma_map->sgl[0]) - g_the_sphcs->inbound_mem_dma_addr) >> PAGE_SHIFT;

		if (inf_devres_is_p2p(devres))
			event_val = devres->p2p_buf.buf_id;

		sphcs_send_event_report_ext(g_the_sphcs,
					    SPH_IPC_CREATE_DEVRES_SUCCESS,
					    event_val,
					    devres->context->protocolID,
					    obj_id_1,
					    obj_id_2);


		// put kref, taken for waiting for runtime response
		inf_devres_put(devres);

		break;

send_rt_devres_destr:
		send_runtime_destroy_devres(devres);
failed_devres:
		sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_CREATE_DEVRES_FAILED,
					eventVal,
					devres->context->protocolID,
					devres->protocolID);
		destroy_devres_on_create_failed(devres);

		break;
	}
	case IOCTL_INF_NETWORK_CREATE_REPLY: {
		struct inf_create_network_reply reply;
		struct inf_devnet *devnet;
		uint8_t event;
		enum event_val eventVal = 0;

		ret = copy_from_user(&reply, (void __user *)arg, sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		devnet = (struct inf_devnet *)(uintptr_t)reply.devnet_drv_handle;
		if (unlikely(!is_inf_devnet_ptr(devnet)))
			return -EINVAL;

		if (unlikely(reply.i_sphcs_err != IOCTL_SPHCS_NO_ERROR)) {
			switch (reply.i_sphcs_err) {
			case IOCTL_SPHCS_NOT_SUPPORTED: {
				eventVal = SPH_IPC_RUNTIME_NOT_SUPPORTED;
				break;
			}
			case IOCTL_SPHCS_INVALID_EXECUTABLE_NETWORK_BINARY: {
				eventVal = SPH_IPC_RUNTIME_INVALID_EXECUTABLE_NETWORK_BINARY;
				break;
			}
			case IOCTL_SPHCS_NO_MEMORY: {
				eventVal = SPH_IPC_NO_MEMORY;
				break;
			}
			case IOCTL_SPHCS_ECC_ALLOC_FAILED: {
				eventVal = SPH_IPC_ECC_ALLOC_FAILED;
				break;
			}
			default: {
				eventVal = SPH_IPC_RUNTIME_FAILED;
			}
			}
			if (!devnet->created)
				event = SPH_IPC_CREATE_DEVNET_FAILED;
			else
				event = SPH_IPC_DEVNET_ADD_RES_FAILED;
			sphcs_send_event_report(g_the_sphcs,
						event,
						eventVal,
						devnet->context->protocolID,
						devnet->protocolID);
			goto failed_devnet;
		}
		SPH_SPIN_LOCK(&devnet->lock);
		devnet->edit_status = CREATED;
		SPH_SPIN_UNLOCK(&devnet->lock);

		// If create was canceled (devnet->destroyed is 1,
		// it can be canceled only by the host at this stage),
		// continue regularly.
		// The devnet will be destroyed on kref put
		// and will send destroy cmd to the runtime.
		if (!devnet->created) {
			devnet->rt_handle = reply.devnet_rt_handle;
			devnet->created = true;
			event = SPH_IPC_CREATE_DEVNET_SUCCESS;

			DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_NETWORK, devnet->context->protocolID, devnet->protocolID,
					SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));
		} else {
			/* mark all added resources as "attached" */
			inf_devnet_attach_all_devres(devnet);
			event = SPH_IPC_DEVNET_ADD_RES_SUCCESS;
		}

		sphcs_send_event_report(g_the_sphcs,
					event,
					eventVal,
					devnet->context->protocolID,
					devnet->protocolID);

		// put kref, taken for waiting for runtime response
		inf_devnet_put(devnet);

		break;

failed_devnet:
		inf_devnet_on_create_or_add_res_failed(devnet);

		break;
	}
	case IOCTL_INF_INFREQ_CREATE_REPLY: {
		struct inf_create_infreq_reply reply;
		struct inf_req *infreq;
		enum event_val eventVal;

		ret = copy_from_user(&reply, (void __user *)arg, sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		infreq = (struct inf_req *)(uintptr_t)reply.infreq_drv_handle;
		if (unlikely(!is_inf_req_ptr(infreq)))
			return -EINVAL;

		if (unlikely(reply.i_sphcs_err != IOCTL_SPHCS_NO_ERROR)) {
			switch (reply.i_sphcs_err) {
			case IOCTL_SPHCS_NOT_SUPPORTED: {
				eventVal = SPH_IPC_RUNTIME_NOT_SUPPORTED;
				break;
			}
			case IOCTL_SPHCS_INFER_MISSING_RESOURCE: {
				eventVal = SPH_IPC_RUNTIME_INFER_MISSING_RESOURCE;
				break;
			}
			case IOCTL_SPHCS_NO_MEMORY: {
				eventVal = SPH_IPC_NO_MEMORY;
				break;
			}
			default: {
				eventVal = SPH_IPC_RUNTIME_FAILED;
			}
			}
			sphcs_send_event_report_ext(g_the_sphcs,
					SPH_IPC_CREATE_INFREQ_FAILED,
					eventVal,
					infreq->devnet->context->protocolID,
					infreq->protocolID,
					infreq->devnet->protocolID);
			goto failed_infreq;
		}

		// If create was canceled (infreq->destroyed is 1, it can be
		// canceled only by the host at this stage) continue regularly.
		// The infreq will be destroyed on kref put
		// and will send destroy cmd to the runtime.
#ifdef _DEBUG
		if (unlikely(infreq->status == CREATED)) {
			sph_log_err(GENERAL_LOG, "Runtime(ctx %u) sent IOCTL_INF_INFREQ_CREATE_REPLY more than once (devnet %u,infreq%u)",
							infreq->devnet->protocolID, infreq->devnet->context->protocolID, infreq->protocolID);
			break;
		}
#endif
		infreq->exec_cmd.infreq_rt_handle = reply.infreq_rt_handle;
		SPH_ASSERT(infreq->status == DMA_COMPLETED);
		infreq->status = CREATED;

		sphcs_send_event_report_ext(g_the_sphcs,
					SPH_IPC_CREATE_INFREQ_SUCCESS,
					0,
					infreq->devnet->context->protocolID,
					infreq->protocolID,
					infreq->devnet->protocolID);
		DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_INF_REQ, infreq->devnet->context->protocolID,
				infreq->protocolID, SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));

		// put kref, taken for waiting for runtime response
		inf_req_put(infreq);

		break;

failed_infreq:
		destroy_infreq_on_create_failed(infreq);

		break;
	}
	case IOCTL_INF_INFREQ_EXEC_DONE: {
		struct inf_infreq_exec_done reply;
		struct inf_req *infreq;
		struct inf_exec_req *req;
		int err = 0;

		ret = copy_from_user(&reply,
				     (void __user *)arg,
				     sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		infreq = (struct inf_req *)(uintptr_t)reply.infreq_drv_handle;
		if (unlikely(!is_inf_req_ptr(infreq)))
			return -EINVAL;

		if (reply.i_error_msg_size > 2*SPH_PAGE_SIZE)
			return -EINVAL;

		req = infreq->active_req;
		SPH_ASSERT(req != NULL);

#ifdef _DEBUG
		if (unlikely(!is_inf_context_ptr(f->private_data)))
			return -EINVAL;
		if (unlikely(reply.infreq_ctx_id !=
		    ((struct inf_context *)f->private_data)->protocolID))
			return -EINVAL;
#endif

		switch (reply.i_sphcs_err) {
		case IOCTL_SPHCS_NO_ERROR: {
			err = 0;
			break;
		}
		case IOCTL_SPHCS_NOT_SUPPORTED: {
			err = -SPHER_NOT_SUPPORTED;
			break;
		}
		case IOCTL_SPHCS_INFER_EXEC_ERROR: {
			err = -SPHER_INFER_EXEC_ERROR;
			break;
		}
		case IOCTL_SPHCS_INFER_ICEDRV_ERROR: {
			err = -SPHER_INFER_ICEDRV_ERROR;
			break;
		}
		case IOCTL_SPHCS_INFER_ICEDRV_ERROR_RESET: {
			err = -SPHER_INFER_ICEDRV_ERROR_RESET;
			break;
		}
		case IOCTL_SPHCS_INFER_ICEDRV_ERROR_CARD_RESET: {
			err = -SPHER_INFER_ICEDRV_ERROR_CARD_RESET;
			break;
		}
		case IOCTL_SPHCS_INFER_SCHEDULE_ERROR: {
			err = -SPHER_INFER_SCHEDULE_ERROR;
			break;
		}
		default:
			err = -EFAULT;
		}
		req->f->complete(req, err,
				 reply.i_error_msg,
				 (reply.i_error_msg_size > 0 ? -reply.i_error_msg_size : 0));

		break;
	}
	case IOCTL_INF_ERROR_EVENT: {
		struct inf_error_ioctl err_ioctl;

		ret = copy_from_user(&err_ioctl,
				     (void __user *)arg,
				     sizeof(err_ioctl));
		if (unlikely(ret != 0))
			return -EIO;

		if (f->private_data == g_the_sphcs->inf_data->daemon)
			handle_daemon_error(&err_ioctl);
		else if (likely(is_inf_context_ptr(f->private_data)))
			handle_runtime_error(f->private_data, &err_ioctl);
		break;
	}
	case IOCTL_INF_DEVNET_RESOURCES_RESERVATION_REPLY: {
		struct inf_devnet_resource_reserve_reply reply;
		struct inf_devnet *devnet;
		uint8_t event;
		enum event_val eventVal = 0;

		ret = copy_from_user(&reply,
				(void __user *)arg,
				sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		devnet = (struct inf_devnet *) (uintptr_t) reply.devnet_drv_handle;
		if (unlikely(!is_inf_devnet_ptr(devnet)))
			return -EINVAL;

		SPH_SPIN_LOCK(&devnet->lock);
		devnet->edit_status = CREATED;
		SPH_SPIN_UNLOCK(&devnet->lock);

		if (unlikely(reply.i_sphcs_err != IOCTL_SPHCS_NO_ERROR)) {
			switch (reply.i_sphcs_err) {
			case IOCTL_SPHCS_INSUFFICIENT_RESOURCES: {
				eventVal = SPH_IPC_DEVNET_RESERVE_INSUFFICIENT_RESOURCES;
				break;
			}
			case IOCTL_SPHCS_TIMED_OUT: {
				eventVal = SPH_IPC_TIMEOUT_EXCEEDED;
				break;
			}
			default: {
				eventVal = SPH_IPC_RUNTIME_FAILED;
			}
			}
			if (reply.reserve_resource)
				event = SPH_IPC_DEVNET_RESOURCES_RESERVATION_FAILED;
			else
				event = SPH_IPC_DEVNET_RESOURCES_RELEASE_FAILED;

		} else {
			if (reply.reserve_resource)
				event = SPH_IPC_DEVNET_RESOURCES_RESERVATION_SUCCESS;
			else
				event = SPH_IPC_DEVNET_RESOURCES_RELEASE_SUCCESS;
		}

		sphcs_send_event_report(g_the_sphcs, event, eventVal,
				devnet->context->protocolID, devnet->protocolID);

		// put kref, taken for waiting for runtime response
		inf_devnet_put(devnet);
		break;
	}
	case IOCTL_INF_DEVNET_RESET_REPLY: {
		struct inf_devnet_reset_reply reply;
		struct inf_devnet *devnet;
		struct inf_cmd_list *cmdlist;

		ret = copy_from_user(&reply,
				(void __user *)arg,
				sizeof(reply));
		if (unlikely(ret != 0))
			return -EIO;

		devnet = (struct inf_devnet *) (uintptr_t) reply.devnet_drv_handle;
		if (unlikely(!is_inf_devnet_ptr(devnet)))
			return -EINVAL;

		if (reply.cmdlist_drv_handle != 0) {
			cmdlist = (struct inf_cmd_list *)(uintptr_t)reply.cmdlist_drv_handle;
			if (unlikely(!is_inf_cmd_ptr(cmdlist)))
				return -EINVAL;
			inf_exec_error_list_devnet_reset_done(&cmdlist->error_list,
							      devnet->protocolID,
							      cmdlist,
							      reply.i_sphcs_err != IOCTL_SPHCS_NO_ERROR);
		} else {
			context = (struct inf_context *)f->private_data;
			if (unlikely(!is_inf_context_ptr(f->private_data)))
				return -EINVAL;
			inf_exec_error_list_devnet_reset_done(&context->error_list,
							      devnet->protocolID,
							      NULL,
							      reply.i_sphcs_err != IOCTL_SPHCS_NO_ERROR);
		}
		break;
	}
	case IOCTL_INF_GET_ALLOC_PGT:
		return handle_get_alloc_pgt((void __user *)arg);
	default:
		return -EINVAL;
	}

	return 0;
}


static unsigned int sphcs_inf_poll(struct file *f, struct poll_table_struct *pt)
{
	unsigned int mask = 0;
	struct inf_daemon *daemon;

	if (unlikely(!is_inf_file(f)))
		return -EINVAL;

	daemon = g_the_sphcs->inf_data->daemon;

	if (f->private_data == daemon)
		mask = inf_cmd_queue_poll(&daemon->cmdq, f, pt);
	else if (is_inf_context_ptr(f->private_data)) {
		struct inf_context *context = (struct inf_context *)f->private_data;

		mask = inf_cmd_queue_poll(&context->cmdq, f, pt);
	}

	return mask;

}


static ssize_t sphcs_inf_read(struct file *f,
			      char __user *buf,
			      size_t       size,
			      loff_t      *off)
{
	struct inf_daemon *daemon;

	if (unlikely(!is_inf_file(f) || !f->private_data))
		return -EINVAL;

	daemon = g_the_sphcs->inf_data->daemon;

	if (f->private_data == daemon) {
		return inf_cmd_queue_read(&daemon->cmdq,
					  buf, size, off);
	} else if (is_inf_context_ptr(f->private_data)) {
		struct inf_context *context = (struct inf_context *)f->private_data;

		return inf_cmd_queue_read(&context->cmdq, buf, size, off);
	}

	return 0;
}

struct context_op_work {
	struct work_struct           work;
	union h2c_InferenceContextOp cmd;
	struct sphcs_cmd_chan       *chan;
};

static void context_op_work_handler(struct work_struct *work)
{
	struct context_op_work *op = container_of(work,
						  struct context_op_work,
						  work);
	struct sphcs *sphcs = g_the_sphcs;
	struct inf_context *context;
	uint8_t event;
	enum event_val val = 0;
	int ret;

	if (!op->cmd.recover && !op->cmd.destroy)
		if (!check_memory_threshold()) {
			event = SPH_IPC_CREATE_CONTEXT_FAILED;
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}

	if (op->cmd.destroy) {
		ret = find_and_destroy_context(sphcs->inf_data, op->cmd.ctxID);
		if (unlikely(ret < 0)) {
			event = SPH_IPC_DESTROY_CONTEXT_FAILED;
			val = SPH_IPC_NO_SUCH_CONTEXT;
			goto send_error;
		}
		if (op->chan)
			sphcs_cmd_chan_put(op->chan);
	} else {
		context = find_and_get_context(sphcs->inf_data, op->cmd.ctxID);
		if (op->cmd.recover) {
			if (unlikely(context == NULL || context->destroyed != 0)) {
				if (context != NULL)
					inf_context_put(context);
				event = SPH_IPC_RECOVER_CONTEXT_FAILED;
				val = SPH_IPC_NO_SUCH_CONTEXT;
				goto send_error;
			}

			recover_context(sphcs, context);
			inf_context_put(context);
			if (op->chan)
				sphcs_cmd_chan_put(op->chan);
		} else {
			if (unlikely(context != NULL)) {
				inf_context_put(context);
				event = SPH_IPC_CREATE_CONTEXT_FAILED;
				val = SPH_IPC_ALREADY_EXIST;
				goto send_error;
			}

			DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_CONTEXT, op->cmd.ctxID, op->cmd.ctxID, SPH_TRACE_OP_STATUS_START, -1, -1));

			val = create_context(sphcs, op->cmd.ctxID, op->cmd.cflags, op->cmd.uid, op->chan);
			if (unlikely(val != 0)) {
				event = SPH_IPC_CREATE_CONTEXT_FAILED;
				goto send_error;
			}
		}
	}

	goto done;

send_error:
	if (op->chan)
		sphcs_cmd_chan_put(op->chan);
	sphcs_send_event_report(sphcs, event, val, op->cmd.ctxID, -1);
done:
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_CONTEXT)(struct sphcs *sphcs,
				     union h2c_InferenceContextOp *cmd)
{
	struct context_op_work *work;
	uint8_t event;

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		if (cmd->recover)
			event = SPH_IPC_RECOVER_CONTEXT_FAILED;
		else if (cmd->destroy)
			event = SPH_IPC_DESTROY_CONTEXT_FAILED;
		else
			event = SPH_IPC_CREATE_CONTEXT_FAILED;
		sphcs_send_event_report(sphcs,
					event,
					SPH_IPC_NO_MEMORY,
					cmd->ctxID,
					-1);
		return;
	}

	work->chan = NULL;
	work->cmd.value = cmd->value;
	INIT_WORK(&work->work, context_op_work_handler);
	queue_work(sphcs->inf_data->inf_wq, &work->work);

	DO_TRACE_IF(!cmd->destroy && !cmd->recover, trace_infer_create(SPH_TRACE_INF_CREATE_CONTEXT,
			cmd->ctxID, cmd->ctxID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));
}

void IPC_OPCODE_HANDLER(CHAN_INF_CONTEXT)(struct sphcs *sphcs,
					  union h2c_ChanInferenceContextOp *cmd)
{
	struct context_op_work *work;
	uint8_t event;
	struct sphcs_cmd_chan *chan;

	chan = sphcs_find_channel(sphcs, cmd->chanID);

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL || chan == NULL)) {
		if (cmd->recover)
			event = SPH_IPC_RECOVER_CONTEXT_FAILED;
		else if (cmd->destroy)
			event = SPH_IPC_DESTROY_CONTEXT_FAILED;
		else
			event = SPH_IPC_CREATE_CONTEXT_FAILED;

		sphcs_send_event_report(sphcs,
					event,
					SPH_IPC_NO_MEMORY,
					cmd->chanID,
					-1);

		if (chan != NULL)
			sphcs_cmd_chan_put(chan);
		if (work != NULL)
			kfree(work);
		return;
	}

	work->cmd.value = 0;
	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = cmd->chanID;
	work->cmd.destroy = cmd->destroy;
	work->cmd.recover = cmd->recover;
	work->cmd.cflags = cmd->cflags;
	work->cmd.uid = chan->uid;
	work->chan = chan;

	INIT_WORK(&work->work, context_op_work_handler);
	queue_work(chan->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy && !cmd->recover, trace_infer_create(SPH_TRACE_INF_CREATE_CONTEXT,
			cmd->chanID, cmd->chanID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));
}

void IPC_OPCODE_HANDLER(SYNC)(struct sphcs   *sphcs,
			      union h2c_Sync *cmd)
{
	struct inf_context *context;

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_INF_SYNC, cmd->contextID, cmd->syncSeq, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	context = find_and_get_context(sphcs->inf_data, cmd->contextID);
	if (unlikely(context == NULL || context->destroyed != 0)) {
		if (context != NULL)
			inf_context_put(context);
		sphcs_send_event_report(sphcs,
					SPH_IPC_CREATE_SYNC_FAILED,
					SPH_IPC_NO_SUCH_CONTEXT,
					cmd->contextID,
					cmd->syncSeq);
		return;
	}

	inf_context_add_sync_point(context, cmd->syncSeq);
	inf_context_put(context);

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_INF_SYNC, cmd->contextID, cmd->syncSeq, SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));
}

void IPC_OPCODE_HANDLER(CHAN_SYNC)(struct sphcs   *sphcs,
				   union h2c_ChanSync *cmd)
{
	struct inf_context *context;

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_INF_SYNC, cmd->chanID, cmd->syncSeq, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL || context->destroyed != 0)) {
		if (context != NULL)
			inf_context_put(context);
		sphcs_send_event_report(sphcs,
					SPH_IPC_CREATE_SYNC_FAILED,
					SPH_IPC_NO_SUCH_CONTEXT,
					cmd->chanID,
					cmd->syncSeq);
		return;
	}

	inf_context_add_sync_point(context, cmd->syncSeq);
	inf_context_put(context);

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_INF_SYNC, cmd->chanID, cmd->syncSeq, SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));
}

struct resource_op_work {
	struct work_struct           work;
	struct inf_context          *context;
	union h2c_InferenceResourceOp cmd;
};

static void resource_op_work_handler(struct work_struct *work)
{
	struct resource_op_work *op = container_of(work,
						   struct resource_op_work,
						   work);
	struct inf_devres *devres;
	uint8_t event;
	enum event_val val = 0;
	uint32_t usage_flags;
	int ret;

	if (!op->cmd.destroy)
		if (!check_memory_threshold()) {
			event = SPH_IPC_CREATE_DEVRES_FAILED;
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}

	if (op->cmd.destroy) {
		ret = inf_context_find_and_destroy_devres(op->context, op->cmd.resID);
		if (unlikely(ret < 0)) {
			event = SPH_IPC_DESTROY_DEVRES_FAILED;
			val = SPH_IPC_NO_SUCH_DEVRES;
			goto send_error;
		}
	} else {
		event = SPH_IPC_CREATE_DEVRES_FAILED;
		if (unlikely(op->context->destroyed != 0)) {
			val = SPH_IPC_NO_SUCH_CONTEXT;
			goto send_error;
		}
		devres = inf_context_find_devres(op->context, op->cmd.resID);
		if (unlikely(devres != NULL)) {
			val = SPH_IPC_ALREADY_EXIST;
			goto send_error;
		}

		DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_DEVRES, op->cmd.ctxID, op->cmd.resID, SPH_TRACE_OP_STATUS_START, -1, -1));

		usage_flags = 0;
		if (op->cmd.is_input || op->cmd.is_network)
			usage_flags |= IOCTL_INF_RES_INPUT;
		if (op->cmd.is_output)
			usage_flags |= IOCTL_INF_RES_OUTPUT;
		if (op->cmd.is_network)
			usage_flags |= IOCTL_INF_RES_NETWORK;
		if (op->cmd.is_force_4G)
			usage_flags |= IOCTL_INF_RES_FORCE_4G_ALLOC;
		if (op->cmd.is_ecc)
			usage_flags |= IOCTL_INF_RES_ECC;
		if (op->cmd.is_p2p_dst)
			usage_flags |= IOCTL_INF_RES_P2P_DST;
		if (op->cmd.is_p2p_src)
			usage_flags |= IOCTL_INF_RES_P2P_SRC;

		ret = inf_context_create_devres(op->context,
						op->cmd.resID,
						op->cmd.size,
						op->cmd.depth,
						op->cmd.align << SPH_PAGE_SHIFT,
						usage_flags,
						&devres);
		if (unlikely(ret < 0)) {
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}
	}

	goto done;

send_error:
	sphcs_send_event_report(g_the_sphcs, event, val, op->cmd.ctxID,	op->cmd.resID);
done:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_RESOURCE)(struct sphcs                  *sphcs,
				 union h2c_InferenceResourceOp     *cmd)
{
	struct resource_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_DEVRES_FAILED;
	else
		event = SPH_IPC_CREATE_DEVRES_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(work->cmd.value, cmd->value, sizeof(cmd->value));
	work->context = context;
	INIT_WORK(&work->work, resource_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy, trace_infer_create(SPH_TRACE_INF_CREATE_DEVRES, cmd->ctxID, cmd->resID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->ctxID, cmd->resID);
}

void IPC_OPCODE_HANDLER(CHAN_INF_RESOURCE)(struct sphcs                  *sphcs,
					   union h2c_ChanInferenceResourceOp     *cmd)
{
	struct resource_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_DEVRES_FAILED;
	else
		event = SPH_IPC_CREATE_DEVRES_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(context->chan == NULL || context->chan->protocolID != cmd->chanID)) {
		inf_context_put(context);
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = cmd->chanID;
	work->cmd.resID = cmd->resID;
	work->cmd.destroy = cmd->destroy;
	work->cmd.is_input = cmd->is_input;
	work->cmd.is_output = cmd->is_output;
	work->cmd.is_network = cmd->is_network;
	work->cmd.is_force_4G = cmd->is_force_4G;
	work->cmd.is_ecc = cmd->is_ecc;
	work->cmd.is_p2p_dst = cmd->is_p2p_dst;
	work->cmd.is_p2p_src = cmd->is_p2p_src;
	work->cmd.depth = cmd->depth;
	work->cmd.size = cmd->size;
	work->cmd.align = cmd->align;
	work->context = context;
	INIT_WORK(&work->work, resource_op_work_handler);
	queue_work(context->chan->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy, trace_infer_create(SPH_TRACE_INF_CREATE_DEVRES, cmd->chanID, cmd->resID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->chanID, cmd->resID);
}

#define POP_VALUE(ptr, type, var)	\
do {					\
	type *__x = (type *)(ptr);	\
					\
	*(var) = *__x;			\
	++__x;				\
	(ptr) = (void *)__x;		\
} while (false)

struct cmdlist_op_work {
	struct work_struct           work;
	struct inf_context          *context;
	union h2c_InferenceCmdListOp cmd;
};

struct cmdlist_dma_data {
	struct inf_cmd_list *cmd;
	uint16_t data_size;
	page_handle dma_page_hndl;
	uint8_t host_page_hdl;
	void *vptr;
	bool is_first;
	bool is_last;
	bool opt_dependencies;
};

static int cmdlist_create_dma_complete(struct sphcs *sphcs,
					void *ctx,
					const void *user_data,
					int status,
					u32 xferTimeUS)
{
	struct cmdlist_dma_data *data = (struct cmdlist_dma_data *)ctx;
	uint8_t event = SPH_IPC_CREATE_CMD_FAILED;
	enum event_val val = SPH_IPC_NO_MEMORY;
	unsigned long flags;
	int ret = 0;
	struct inf_cmd_list *cmd = data->cmd;
	struct inf_copy *copy;
	struct inf_cpylst *cpylst;
	struct inf_devnet *devnet;
	struct inf_req *infreq;
	uint16_t protID;
	uint32_t cmdlist_len, cmd_index;
	size_t size;
	uint8_t cmd_type, priority = 0;
	uint16_t ncopies, batchSize = 0;
	uint8_t debugOn = 0, collectInfo = 0, sched_params_are_null;
	uint8_t *begin;

	if (cmd->context->chan != NULL)
		sphcs_cmd_chan_update_cmd_head(cmd->context->chan, 0, PAGE_SIZE);

	if (unlikely(status == SPHCS_DMA_STATUS_FAILED)) {
		val = SPH_IPC_DMA_ERROR;
		ret = -EFAULT;
		goto destroy_cmd;
	}
	if (unlikely(cmd->destroyed != 0)) {
		val = SPH_IPC_NO_SUCH_CMD;
		ret = -1;
		goto done;
	}

	if (data->is_first) {
		POP_VALUE(data->vptr, uint32_t, &cmdlist_len);
		data->data_size -= sizeof(uint32_t);
		if (unlikely(cmdlist_len == 0)) {
			val = SPH_IPC_RUNTIME_NOT_SUPPORTED;
			ret = -EINVAL;
			goto destroy_cmd;
		}
		cmd->req_list = kmalloc_array(cmdlist_len, sizeof(struct inf_exec_req), GFP_KERNEL);
		if (unlikely(cmd->req_list == NULL)) {
			ret = -ENOMEM;
			goto destroy_cmd;
		}
		// Makr as not inited
		for (cmd_index = 0; cmd_index < cmdlist_len; ++cmd_index)
			cmd->req_list[cmd_index].f = NULL;
	}

	while (data->data_size > 0) {
		begin = data->vptr;
		POP_VALUE(data->vptr, uint32_t, &cmd_index);//not used here
		POP_VALUE(data->vptr, uint8_t, &cmd_type);
		switch (cmd_type) {
		case CMDLIST_CMD_COPY:
			POP_VALUE(data->vptr, uint16_t, &protID);
			copy = inf_context_find_and_get_copy(cmd->context, protID);
			if (copy == NULL) {
				val = SPH_IPC_NO_SUCH_COPY;
				ret = -ENOENT;
				goto destroy_cmd;
			}
			POP_VALUE(data->vptr, uint8_t, &priority);
			POP_VALUE(data->vptr, uint64_t, &size);
			if (cmd->num_left == 0) {// standalone copy
				//kref is taken in find
				inf_copy_req_init(&cmd->req_list[cmd->num_reqs], copy, cmd, size, priority);
				++cmd->num_reqs;
			} else { // copy in cpylst
				ret = inf_cpylst_add_copy(cmd->req_list[cmd->num_reqs].cpylst, copy, size, priority);
				inf_copy_put(copy);
				if (ret < 0)
					goto destroy_cmd;

				DO_TRACE(trace_infer_create(SPH_TRACE_INF_ADD_TO_COPY_LIST,
						copy->context->protocolID,
						copy->protocolID, SPH_TRACE_OP_STATUS_QUEUED, cmd->protocolID, cmd->num_reqs));

				DO_TRACE(trace_infer_create(SPH_TRACE_INF_ADD_TO_COPY_LIST,
						copy->context->protocolID,
						copy->protocolID, SPH_TRACE_OP_STATUS_START, cmd->protocolID, cmd->num_reqs));

				DO_TRACE(trace_infer_create(SPH_TRACE_INF_ADD_TO_COPY_LIST,
						copy->context->protocolID,
						copy->protocolID, SPH_TRACE_OP_STATUS_COMPLETE, cmd->protocolID, cmd->num_reqs));

				//TODO CPYLST treat different priorities
				if (priority != 0 && cmd->req_list[cmd->num_reqs].priority == 0)
					cmd->req_list[cmd->num_reqs].priority = priority;

				--cmd->num_left;
				if (cmd->num_left == 0) {//after finilize is done
					cmd->req_list[cmd->num_reqs].size = cmd->req_list[cmd->num_reqs].cpylst->size;
					cmd->req_list[cmd->num_reqs].lli_addr = cmd->req_list[cmd->num_reqs].cpylst->lli_addr;
					++cmd->num_reqs;

					DO_TRACE(trace_cpylist_create(SPH_TRACE_OP_STATUS_COMPLETE,
							 cmd->context->protocolID, cmd->protocolID, cmd->num_reqs));
				}
			}
			++cmd->edits_idx;//count max edits possible
			break;
		case CMDLIST_CMD_INFREQ:
			SPH_ASSERT(cmd->num_left == 0);
			POP_VALUE(data->vptr, uint16_t, &protID);
			devnet = inf_context_find_and_get_devnet(cmd->context, protID, true, true);
			if (unlikely(devnet == NULL)) {
				val = SPH_IPC_NO_SUCH_NET;
				ret = -ENOENT;
				goto destroy_cmd;
			}
			POP_VALUE(data->vptr, uint16_t, &protID);
			infreq = inf_devnet_find_and_get_infreq(devnet, protID);
			inf_devnet_put(devnet);
			if (infreq == NULL) {
				val = SPH_IPC_NO_SUCH_INFREQ;
				ret = -ENOENT;
				goto destroy_cmd;
			}
			POP_VALUE(data->vptr, uint8_t, &sched_params_are_null);
			if (sched_params_are_null == 0) {
				POP_VALUE(data->vptr, uint16_t, &batchSize);
				POP_VALUE(data->vptr, uint8_t, &priority);
				POP_VALUE(data->vptr, uint8_t, &debugOn);
				POP_VALUE(data->vptr, uint8_t, &collectInfo);
			} else {
				priority = 0;
			}
			infreq_req_init(&cmd->req_list[cmd->num_reqs],
					infreq,
					cmd,
					priority,
					sched_params_are_null != 0,
					batchSize,
					debugOn,
					collectInfo);
			++cmd->num_reqs;
			++cmd->edits_idx;//count max edits possible
			break;
		case CMDLIST_CMD_COPYLIST:
			SPH_ASSERT(cmd->num_left == 0);
			POP_VALUE(data->vptr, uint16_t, &ncopies);
			SPH_ASSERT(ncopies > 0);

			DO_TRACE(trace_cpylist_create(SPH_TRACE_OP_STATUS_QUEUED, cmd->context->protocolID, cmd->protocolID, cmd->num_reqs));
			DO_TRACE(trace_cpylist_create(SPH_TRACE_OP_STATUS_START, cmd->context->protocolID, cmd->protocolID, cmd->num_reqs));

			ret = inf_cpylst_create(cmd, cmd->num_reqs, ncopies, &cpylst);
			if (ret < 0) {
				ret = -ENOMEM;
				goto destroy_cmd;
			}
			inf_cpylst_req_init(&cmd->req_list[cmd->num_reqs], cpylst, cmd);
			cmd->num_left = ncopies;
			break;
		default:
			//NOT supported
			val = SPH_IPC_RUNTIME_NOT_SUPPORTED;
			ret = -EINVAL;
			goto destroy_cmd;
		}
		data->data_size -= ((uint8_t *)data->vptr - begin);
	}

	// Do not send reply and mark create completed if not last create
	// packet.
	if (!data->is_last)
		goto done;

	cmd->edits = kmalloc_array(cmd->edits_idx, sizeof(struct req_params), GFP_KERNEL);
	if (unlikely(cmd->edits == NULL)) {
		ret = -ENOMEM;
		goto destroy_cmd;
	}
	cmd->edits_idx = 0;

	SPH_SPIN_LOCK_IRQSAVE(&cmd->lock_irq, flags);
	if (unlikely(cmd->destroyed != 0)) {
		SPH_SPIN_UNLOCK_IRQRESTORE(&cmd->lock_irq, flags);
		goto done;
	}
	SPH_ASSERT(cmd->status == CREATE_STARTED);

	// skip this stage cmd->status = DMA_COMPLETED;
	cmd->status = CREATED;
	// ready to schedule
	SPH_ASSERT(cmd->num_left == 0);
	SPH_SPIN_UNLOCK_IRQRESTORE(&cmd->lock_irq, flags);

	if (data->opt_dependencies)
		inf_cmd_optimize_group_devres(cmd);

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_COMMAND_LIST,
			cmd->context->protocolID,
			cmd->protocolID,
			SPH_TRACE_OP_STATUS_COMPLETE, -1, -1));

	event = SPH_IPC_CREATE_CMD_SUCCESS;
	val = 0;

	goto send_report;

destroy_cmd:
	destroy_cmd_on_create_failed(cmd);
send_report:
	sphcs_send_event_report(g_the_sphcs,
				event,
				val,
				cmd->context->protocolID,
				cmd->protocolID);
done:
	if (cmd->context->chan == NULL) {
		union c2h_DmaPageHandleFree free_page_msg = { .value = 0 };

		free_page_msg.opcode = SPH_IPC_C2H_OP_DMA_PAGE_HANDLE_FREE;
		free_page_msg.host_page_hndl = data->host_page_hdl;
		sphcs_msg_scheduler_queue_add_msg(g_the_sphcs->public_respq, &free_page_msg.value, 1);
	}

	// put kref for DMA
	inf_cmd_put(cmd);
	dma_page_pool_set_page_free(sphcs->dma_page_pool,
				    data->dma_page_hndl);
	kfree(data);
	return ret;
}

static void cmdlist_op_work_handler(struct work_struct *work)
{
	struct cmdlist_op_work *op = container_of(work,
						  struct cmdlist_op_work,
						  work);
	struct inf_cmd_list *cmd;
	uint8_t event;
	enum event_val val;
	int ret;
	struct cmdlist_dma_data *dma_data;
	dma_addr_t dma_addr;
	dma_addr_t host_dma_addr;

	if (op->cmd.destroy) {
		ret = inf_context_find_and_destroy_cmd(op->context, op->cmd.cmdID);
		if (unlikely(ret < 0)) {
			event = SPH_IPC_DESTROY_CMD_FAILED;
			val = SPH_IPC_NO_SUCH_CMD;
			goto send_error;
		}
		goto done;
	}

	if (op->context->chan != NULL) {
		struct sphcs_host_rb *cmd_data_rb = &op->context->chan->h2c_rb[0];
		u32 host_chunk_size;
		int n;

		/* need to advance h2c ring buffer by one page */
		host_rb_update_free_space(cmd_data_rb, SPH_PAGE_SIZE);
		n = host_rb_get_avail_space(cmd_data_rb,
					    SPH_PAGE_SIZE,
					    1,
					    &host_dma_addr,
					    &host_chunk_size);

		SPH_ASSERT(n == 1);
		SPH_ASSERT((host_dma_addr & SPH_IPC_DMA_ADDR_ALIGN_MASK) == 0);

		host_rb_update_avail_space(cmd_data_rb, SPH_PAGE_SIZE);
	}

	event = SPH_IPC_CREATE_CMD_FAILED;
	if (unlikely(op->context->destroyed != 0)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}
	cmd = inf_context_find_cmd(op->context, op->cmd.cmdID);
	if (op->cmd.is_first) {
		if (unlikely(cmd != NULL)) {
			val = SPH_IPC_ALREADY_EXIST;
			goto send_error;
		}

		DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_COMMAND_LIST, op->context->protocolID, op->cmd.cmdID, SPH_TRACE_OP_STATUS_START, -1, -1));

		ret = inf_context_create_cmd(op->context, op->cmd.cmdID, &cmd);
		if (unlikely(ret < 0)) {
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}

	} else if (unlikely(cmd == NULL)) {
		val = SPH_IPC_NO_SUCH_CMD;
		goto send_error;
	}

	SPH_SPIN_LOCK(&op->context->lock);
	if (op->cmd.is_first)
		hash_add(op->context->cmd_hash, &cmd->hash_node, cmd->protocolID);

	SPH_ASSERT(cmd->status != CREATED);
	// get kref to prevent the cmd list to be destroyed,
	// when it is waiting for dma to complete
	inf_cmd_get(cmd);
	SPH_SPIN_UNLOCK(&op->context->lock);

	dma_data = kmalloc(sizeof(struct cmdlist_dma_data), GFP_KERNEL);
	if (unlikely(dma_data == NULL)) {
		val = SPH_IPC_NO_MEMORY;
		goto destroy_cmd;
	}

	dma_data->cmd = cmd;
	dma_data->data_size = op->cmd.size;
	dma_data->is_last = op->cmd.is_last;
	dma_data->is_first = op->cmd.is_first;
	dma_data->opt_dependencies = op->cmd.opt_dependencies;
	SPH_ASSERT(dma_data->data_size <= SPH_PAGE_SIZE);

	ret = dma_page_pool_get_free_page(g_the_sphcs->dma_page_pool,
					  &dma_data->dma_page_hndl,
					  &dma_data->vptr,
					  &dma_addr);
	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto free_dma_data;
	}

	if (op->context->chan == NULL) {
		host_dma_addr = SPH_IPC_DMA_PFN_TO_ADDR(op->cmd.host_pfn);
		dma_data->host_page_hdl = op->cmd.host_page_hndl;
	}

	ret = sphcs_dma_sched_start_xfer_single(g_the_sphcs->dmaSched,
						op->context->chan ? &op->context->chan->h2c_dma_desc :
								    &cmd->h2c_dma_desc,
						host_dma_addr,
						dma_addr,
						dma_data->data_size,
						cmdlist_create_dma_complete,
						dma_data,
						NULL,
						0);

	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto free_page;
	}

	goto done;

free_page:
	dma_page_pool_set_page_free(g_the_sphcs->dma_page_pool,
				    dma_data->dma_page_hndl);
free_dma_data:
	kfree(dma_data);
destroy_cmd:
	destroy_cmd_on_create_failed(cmd);
	// put kref for DMA
	inf_cmd_put(cmd);
send_error:
	if (op->context->chan != NULL)
		sphcs_cmd_chan_update_cmd_head(op->context->chan, 0, PAGE_SIZE);
	sphcs_send_event_report(g_the_sphcs, event, val, op->cmd.ctxID,	op->cmd.cmdID);
done:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_CMDLIST)(struct sphcs                  *sphcs,
				     union h2c_InferenceCmdListOp  *cmd)
{
	struct cmdlist_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_CMD_FAILED;
	else
		event = SPH_IPC_CREATE_CMD_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(work->cmd.value, cmd->value, sizeof(cmd->value));
	work->context = context;
	INIT_WORK(&work->work, cmdlist_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_COMMAND_LIST, cmd->ctxID, cmd->cmdID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->ctxID, cmd->cmdID);
}

void IPC_OPCODE_HANDLER(CHAN_INF_CMDLIST)(struct sphcs                      *sphcs,
					  union h2c_ChanInferenceCmdListOp  *cmd)
{
	struct cmdlist_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_CMD_FAILED;
	else
		event = SPH_IPC_CREATE_CMD_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = cmd->chanID;
	work->cmd.cmdID = cmd->cmdID;
	work->cmd.destroy = cmd->destroy;
	work->cmd.host_pfn = 0;
	work->cmd.size = cmd->size;
	work->cmd.is_first = cmd->is_first;
	work->cmd.is_last = cmd->is_last;
	work->cmd.opt_dependencies = cmd->opt_dependencies;
	work->context = context;
	INIT_WORK(&work->work, cmdlist_op_work_handler);
	queue_work(context->chan->wq, &work->work);

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_COMMAND_LIST, cmd->chanID, cmd->cmdID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->chanID, cmd->cmdID);
}

struct network_op_work {
	struct work_struct work;
	struct inf_context *context;
	union h2c_InferenceNetworkOp cmd;
	int rbID;
	uint32_t start_res_idx;
};

struct network_dma_data {
	bool create;
	bool chained;
	uint32_t num_res;
	uint32_t curr_num_res;
	uint16_t config_data_size;
	int rbID;

	page_handle host_dma_page_hndl;
	dma_addr_t host_dma_addr;

	page_handle dma_page_hndl;
	void *vptr;
	dma_addr_t dma_addr;
	struct inf_devnet *devnet;
};

struct network_edit_data {
	struct inf_create_network cmd;
};

static int network_op_dma_complete(struct sphcs *sphcs,
				   void *ctx,
				   const void *user_data,
				   int status,
				   u32 xferTimeUS)
{
	struct network_dma_data *data = (struct network_dma_data *)ctx;
	uint8_t event;
	enum event_val val;
	int ret;
	uint16_t *packet_ptr;
	struct inf_devnet *devnet = data->devnet;
	struct network_edit_data *edit_data = (struct network_edit_data *)devnet->edit_data;
	struct inf_devres *devres;
	uint32_t cmd_size;
	uint64_t *int64ptr;
	uint32_t j, max_entries_per_page = data->chained ? (SPH_PAGE_SIZE - sizeof(u64)) / sizeof(uint16_t) : data->num_res;


	if (devnet->context->chan != NULL) {
		sphcs_cmd_chan_update_cmd_head(devnet->context->chan,
					       data->rbID,
					       SPH_PAGE_SIZE);
		max_entries_per_page = data->chained ? SPH_PAGE_SIZE / sizeof(uint16_t) : data->num_res;
	} else {
		max_entries_per_page = data->chained ? (SPH_PAGE_SIZE - sizeof(u64)) / sizeof(uint16_t) : data->num_res;
	}

	if (unlikely(status == SPHCS_DMA_STATUS_FAILED)) {
		val = SPH_IPC_DMA_ERROR;
		ret = -EFAULT;
		goto send_error;
	}
	if (unlikely(devnet->destroyed != 0)) {
		ret = -1;
		goto done;
	}

	packet_ptr = (uint16_t *)data->vptr;
	int64ptr = (uint64_t *)(&edit_data->cmd + 1);
	int64ptr = int64ptr + data->curr_num_res;
	for (j = 0; data->curr_num_res < data->num_res && j < max_entries_per_page; ++j) {
		devres = inf_context_find_and_get_devres(devnet->context,
							  *(packet_ptr++));
		if (unlikely(devres == NULL)) {
			val = SPH_IPC_NO_SUCH_DEVRES;
			ret = -ENXIO;
			goto delete_devnet;
		}

		ret = inf_devnet_add_devres(devnet, devres);
		inf_devres_put(devres);
		if (unlikely(ret)) {
			val = SPH_IPC_NO_MEMORY;
			goto delete_devnet;
		}
		*(int64ptr++) = devres->rt_handle;
		data->curr_num_res++;
	}

	if (data->curr_num_res < data->num_res) {
		u64 host_pfn;
		uint8_t host_page_handle;
		uint16_t dma_transfer_size;
		uint64_t *page_data_ptr = (uint64_t *)packet_ptr;

		/* with chan protocol we will get another command to
		 * start the next dma
		 */
		if (devnet->context->chan != NULL) {
			ret = 0;
			goto done_curr_packet;
		}

		host_pfn = *page_data_ptr & 0x00001FFFFFFFFFFF;
		host_page_handle = (*page_data_ptr & 0xFF00000000000000) >> 56;
		data->host_dma_page_hndl = host_page_handle;
		data->host_dma_addr = SPH_IPC_DMA_PFN_TO_ADDR(host_pfn);

		if (data->num_res - data->curr_num_res < max_entries_per_page)
			dma_transfer_size = (data->num_res - data->curr_num_res) * sizeof(uint16_t) + data->config_data_size;
		else
			dma_transfer_size = SPH_PAGE_SIZE;

		//Call dma transfer for next page
		ret = sphcs_dma_sched_start_xfer_single(g_the_sphcs->dmaSched,
							&g_dma_desc_h2c_normal,
							data->host_dma_addr,
							data->dma_addr,
							dma_transfer_size,
							network_op_dma_complete,
							data,
							NULL,
							0);


		if (unlikely(ret < 0)) {
			sph_log_err(GENERAL_LOG, "dma xfer single failed with out of memory\n");
			val = SPH_IPC_NO_MEMORY;
			goto delete_devnet;
		}

		return ret;
	}

	cmd_size = sizeof(edit_data->cmd) +
		   data->num_res * sizeof(uint64_t) +
		   data->config_data_size;

	edit_data->cmd.devnet_drv_handle = (uint64_t)devnet;
	edit_data->cmd.devnet_rt_handle = (uint64_t)devnet->rt_handle;
	edit_data->cmd.num_devres_rt_handles = data->num_res;
	edit_data->cmd.config_data_size = data->config_data_size;
	edit_data->cmd.network_id = (uint32_t)devnet->protocolID;
	if (data->config_data_size > 0)
		memcpy(int64ptr, packet_ptr, data->config_data_size);

	SPH_SPIN_LOCK(&devnet->lock);
	if (unlikely(devnet->destroyed != 0)) {
		SPH_SPIN_UNLOCK(&devnet->lock);
		goto done;
	}
	SPH_ASSERT(devnet->edit_status == CREATE_STARTED);
	devnet->edit_status = DMA_COMPLETED;
	// get kref for RT
	inf_devnet_get(devnet);
	SPH_SPIN_UNLOCK(&devnet->lock);

	ret = inf_cmd_queue_add(&devnet->context->cmdq,
				SPHCS_RUNTIME_CMD_CREATE_NETWORK,
				&edit_data->cmd,
				cmd_size,
				NULL, NULL);
	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto delete_devnet;
	}

	goto done;

delete_devnet:
	inf_devnet_on_create_or_add_res_failed(data->devnet);
send_error:
	if (data->create)
		event = SPH_IPC_CREATE_DEVNET_FAILED;
	else
		event = SPH_IPC_DEVNET_ADD_RES_FAILED;

	sphcs_send_event_report(g_the_sphcs,
				event,
				val,
				devnet->context->protocolID,
				devnet->protocolID);
done:
	kfree(devnet->edit_data);
	devnet->edit_data = NULL;
done_curr_packet:
	// put kref for DMA
	inf_devnet_put(devnet);
	dma_page_pool_set_page_free(sphcs->dma_page_pool,
				    data->dma_page_hndl);
	kfree(data);
	return ret;
}

static void network_op_work_handler(struct work_struct *work)
{
	struct network_op_work
	*op = container_of(work,
			struct network_op_work,
			work);
	struct network_dma_data *dma_data;
	struct inf_devnet *devnet;
	uint16_t config_data_size;
	dma_addr_t host_dma_addr;
	uint8_t event;
	enum event_val val;
	int ret;

	if (op->cmd.destroy) {
		ret = inf_context_find_and_destroy_devnet(op->context, op->cmd.netID);
		if (unlikely(ret < 0)) {
			event = SPH_IPC_DESTROY_DEVNET_FAILED;
			val = SPH_IPC_NO_SUCH_NET;
			goto send_error;
		}
		goto done;
	} else {
		if (op->context->chan == NULL)
			host_dma_addr = SPH_IPC_DMA_PFN_TO_ADDR(op->cmd.host_pfn);
		else {
			struct sphcs_host_rb *cmd_data_rb = &op->context->chan->h2c_rb[op->rbID];
			u32 host_chunk_size;
			int n;

			/* need to advance h2c ring buffer by one page */
			host_rb_update_free_space(cmd_data_rb, SPH_PAGE_SIZE);
			n = host_rb_get_avail_space(cmd_data_rb,
						    SPH_PAGE_SIZE,
						    1,
						    &host_dma_addr,
						    &host_chunk_size);

			SPH_ASSERT(n == 1);
			SPH_ASSERT((host_dma_addr & SPH_IPC_DMA_ADDR_ALIGN_MASK) == 0);

			host_rb_update_avail_space(cmd_data_rb, SPH_PAGE_SIZE);
		}

		if (!check_memory_threshold()) {
			event = SPH_IPC_CREATE_DEVNET_FAILED;
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}
	}

	if (op->cmd.create)
		event = SPH_IPC_CREATE_DEVNET_FAILED;
	else
		event = SPH_IPC_DEVNET_ADD_RES_FAILED;

	if (unlikely(op->context->destroyed != 0)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (op->cmd.create && op->start_res_idx == 0) {
		devnet = inf_context_find_devnet(op->context, op->cmd.netID);
		if (unlikely(devnet != NULL)) {
			val = SPH_IPC_ALREADY_EXIST;
			goto send_error;
		}

		DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_NETWORK,
			 op->cmd.ctxID, op->cmd.netID,
			 SPH_TRACE_OP_STATUS_START, -1, -1));

		ret = inf_devnet_create(op->cmd.netID,
					op->context,
					&devnet);
		if (unlikely(ret)) {
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}
	} else {
		devnet = inf_context_find_and_get_devnet(op->context, op->cmd.netID, true, false);
		if (unlikely(devnet == NULL)) {
			val = SPH_IPC_NO_SUCH_NET;
			goto send_error;
		}

		if (unlikely(op->cmd.create && devnet->created)) {
			val = SPH_IPC_ALREADY_EXIST;
			inf_devnet_put(devnet);
			goto send_error;
		}
	}

	SPH_SPIN_LOCK(&devnet->lock);
	/* Add resource operation cannot run while network editing still in progress */
	if (unlikely((!op->cmd.create) && (devnet->edit_status != CREATED))) {
		SPH_SPIN_UNLOCK(&devnet->lock);
		inf_devnet_put(devnet);
		val = SPH_IPC_NOT_SUPPORTED;
		goto send_error;
	}
	devnet->edit_status = CREATE_STARTED;
	// kref for DMA is taken in find or in create
	SPH_SPIN_UNLOCK(&devnet->lock);

	config_data_size = op->cmd.size + 1
			   - (op->cmd.num_res * sizeof(uint16_t));

	if (op->start_res_idx == 0) {
		if (devnet->edit_data != NULL) {
			val = SPH_IPC_DEVNET_EDIT_BUSY;
			goto destroy_devnet;
		}

		devnet->edit_data = kmalloc(sizeof(struct network_edit_data) +
					    op->cmd.num_res * sizeof(uint64_t) + config_data_size,
					    GFP_KERNEL);
		if (!devnet->edit_data) {
			val = SPH_IPC_NO_MEMORY;
			goto destroy_devnet;
		}
	} else if (devnet->edit_data == NULL) {
		val = SPH_IPC_DEVNET_EDIT_ERROR;
		goto destroy_devnet;
	}

	dma_data = kmalloc(sizeof(struct network_dma_data), GFP_KERNEL);
	if (unlikely(dma_data == NULL)) {
		val = SPH_IPC_NO_MEMORY;
		goto destroy_devnet;
	}

	dma_data->devnet = devnet;
	dma_data->create = op->cmd.create;
	dma_data->num_res = op->cmd.num_res;
	dma_data->curr_num_res = op->start_res_idx;
	dma_data->config_data_size = config_data_size;
	dma_data->chained = op->cmd.chained;
	dma_data->host_dma_addr = host_dma_addr;

	ret = dma_page_pool_get_free_page(g_the_sphcs->dma_page_pool,
					  &dma_data->dma_page_hndl,
					  &dma_data->vptr,
					  &dma_data->dma_addr);
	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto free_dma_data;
	}

	if (op->context->chan == NULL)
		dma_data->host_dma_page_hndl = op->cmd.dma_page_hndl;
	else
		dma_data->rbID = op->rbID;

	if (unlikely(devnet->destroyed)) { // RT died
		val = SPH_IPC_CONTEXT_BROKEN;
		goto free_page;
	}
	ret = sphcs_dma_sched_start_xfer_single(g_the_sphcs->dmaSched,
						op->context->chan ? &op->context->chan->h2c_dma_desc :
								    &g_dma_desc_h2c_normal,
						dma_data->host_dma_addr,
						dma_data->dma_addr,
						op->cmd.chained ? SPH_PAGE_SIZE : op->cmd.size + 1,
						network_op_dma_complete,
						dma_data,
						NULL,
						0);

	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto free_page;
	}

	goto done;

free_page:
	dma_page_pool_set_page_free(g_the_sphcs->dma_page_pool,
				    dma_data->dma_page_hndl);
free_dma_data:
	kfree(dma_data);
destroy_devnet:
	inf_devnet_on_create_or_add_res_failed(devnet);
	// put kref for DMA
	inf_devnet_put(devnet);
send_error:
	if (!op->cmd.destroy && op->context->chan != NULL)
		sphcs_cmd_chan_update_cmd_head(op->context->chan,
					       op->rbID,
					       SPH_PAGE_SIZE);
	sphcs_send_event_report(g_the_sphcs, event, val,
				op->cmd.ctxID, op->cmd.netID);
done:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_NETWORK)(struct sphcs *sphcs, union h2c_InferenceNetworkOp *cmd)
{
	struct network_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_DEVNET_FAILED;
	else if (cmd->create)
		event = SPH_IPC_CREATE_DEVNET_FAILED;
	else
		event = SPH_IPC_DEVNET_ADD_RES_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(work->cmd.value, cmd->value, sizeof(work->cmd.value));
	work->context = context;
	INIT_WORK(&work->work, network_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy, trace_infer_create(SPH_TRACE_INF_CREATE_NETWORK, cmd->ctxID, cmd->netID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->ctxID, cmd->netID);
}

void IPC_OPCODE_HANDLER(CHAN_INF_NETWORK)(struct sphcs *sphcs, union h2c_ChanInferenceNetworkOp *cmd)
{
	struct network_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_DEVNET_FAILED;
	else if (cmd->create)
		event = SPH_IPC_CREATE_DEVNET_FAILED;
	else
		event = SPH_IPC_DEVNET_ADD_RES_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = context->chan->protocolID;
	work->cmd.netID = cmd->netID;
	work->cmd.destroy = cmd->destroy;
	work->cmd.create = cmd->create;
	work->cmd.num_res = cmd->num_res;
	work->cmd.dma_page_hndl = 0;
	work->cmd.size = cmd->size;
	work->cmd.chained = cmd->chained;
	work->cmd.host_pfn = 0;
	work->rbID = cmd->rbID;
	work->start_res_idx = cmd->start_res_idx;
	work->context = context;
	INIT_WORK(&work->work, network_op_work_handler);
	queue_work(context->chan->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy, trace_infer_create(SPH_TRACE_INF_CREATE_NETWORK, cmd->chanID, cmd->netID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->chanID, cmd->netID);
}

struct copy_op_work {
	struct work_struct           work;
	struct inf_context          *context;
	union h2c_InferenceCopyOp    cmd;
	bool is_subres_copy;
};

static void copy_op_work_handler(struct work_struct *work)
{
	struct copy_op_work *op = container_of(work,
					       struct copy_op_work,
					       work);
	struct inf_devres *devres;
	struct inf_copy *copy;
	uint8_t event;
	enum event_val val;
	int ret;

	if (!op->cmd.destroy)
		if (!check_memory_threshold()) {
			event = SPH_IPC_CREATE_COPY_FAILED;
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}

	if (op->cmd.destroy) {
		ret = inf_context_find_and_destroy_copy(op->context, op->cmd.protCopyID);
		if (unlikely(ret < 0)) {
			event = SPH_IPC_DESTROY_COPY_FAILED;
			val = SPH_IPC_NO_SUCH_COPY;
			goto send_error;
		}
	} else { // Create copy
		event = SPH_IPC_CREATE_COPY_FAILED;

		if (unlikely(op->context->destroyed != 0)) {
			val = SPH_IPC_NO_SUCH_CONTEXT;
			goto send_error;
		}

		devres = inf_context_find_and_get_devres(op->context, op->cmd.protResID);
		if (unlikely(devres == NULL)) {
			val = SPH_IPC_NO_SUCH_DEVRES;
			goto send_error;
		}

		copy = inf_context_find_copy(op->context, op->cmd.protCopyID);
		if (unlikely(copy != NULL)) {
			inf_devres_put(devres);
			val = SPH_IPC_ALREADY_EXIST;
			goto send_error;
		}

		DO_TRACE_IF(!op->is_subres_copy, trace_infer_create((op->cmd.c2h ? SPH_TRACE_INF_CREATE_C2H_COPY_HANDLE : SPH_TRACE_INF_CREATE_H2C_COPY_HANDLE),
				op->cmd.ctxID, op->cmd.protCopyID, SPH_TRACE_OP_STATUS_START, -1, -1));

		if (op->cmd.d2d) {
			ret = inf_d2d_copy_create(op->cmd.protCopyID,
						  op->context,
						  devres,
						  SPH_IPC_DMA_PFN_TO_ADDR(op->cmd.hostPtr),
						  &copy);
		} else {
			ret = inf_copy_create(op->cmd.protCopyID,
						      op->context,
						      devres,
						      op->context->chan == NULL ? SPH_IPC_DMA_PFN_TO_ADDR(op->cmd.hostPtr) : op->cmd.hostPtr,
						      op->cmd.c2h,
						      op->is_subres_copy,
						      &copy);
		}
		inf_devres_put(devres);
		if (unlikely(ret < 0)) {
			if (ret == -ENOENT)
				val = SPH_IPC_NO_SUCH_HOSTRES;
			else
				val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}
	}

	goto done;

send_error:
	sphcs_send_event_report(g_the_sphcs, event, val, op->cmd.ctxID, op->cmd.protCopyID);
done:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(COPY_OP)(struct sphcs                  *sphcs,
				 union h2c_InferenceCopyOp     *cmd)
{
	struct copy_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_COPY_FAILED;
	else
		event = SPH_IPC_CREATE_COPY_FAILED;

	context = find_and_get_context(g_the_sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(&work->cmd, cmd, sizeof(*cmd));
	work->context = context;
	INIT_WORK(&work->work, copy_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy,
			trace_infer_create((cmd->c2h ? SPH_TRACE_INF_CREATE_C2H_COPY_HANDLE : SPH_TRACE_INF_CREATE_H2C_COPY_HANDLE),
					cmd->ctxID, cmd->protCopyID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report(g_the_sphcs, event, val, cmd->ctxID, cmd->protCopyID);
}

void IPC_OPCODE_HANDLER(CHAN_COPY_OP)(struct sphcs                  *sphcs,
				      union h2c_ChanInferenceCopyOp *cmd)
{
	struct copy_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_COPY_FAILED;
	else
		event = SPH_IPC_CREATE_COPY_FAILED;

	context = find_and_get_context(g_the_sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(context->chan == NULL || context->chan->protocolID != cmd->chanID)) {
		inf_context_put(context);
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memset(&work->cmd, 0, sizeof(work->cmd));
	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = cmd->chanID;
	work->cmd.protResID = cmd->protResID;
	work->cmd.protCopyID = cmd->protCopyID;
	work->cmd.d2d = cmd->d2d;
	work->cmd.c2h = cmd->c2h;
	work->cmd.destroy = cmd->destroy;
	work->cmd.hostPtr = cmd->hostres;
	work->context = context;
	work->is_subres_copy = cmd->subres_copy;
	INIT_WORK(&work->work, copy_op_work_handler);
	queue_work(context->chan->wq, &work->work);

	DO_TRACE_IF(!work->is_subres_copy && !cmd->destroy,
			trace_infer_create((cmd->c2h ? SPH_TRACE_INF_CREATE_C2H_COPY_HANDLE : SPH_TRACE_INF_CREATE_H2C_COPY_HANDLE),
					cmd->chanID, cmd->protCopyID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));
	return;

send_error:
	sphcs_send_event_report(g_the_sphcs, event, val, cmd->chanID, cmd->protCopyID);
}

static void send_exec_error_cmd_error(union h2c_ExecErrorList   *cmd,
				      uint16_t                   error_val,
				      uint16_t                   pkt_size)
{
	union c2h_ExecErrorList msg;

	msg.value = 0;
	msg.opcode = SPH_IPC_C2H_OP_CHAN_EXEC_ERROR_LIST;
	msg.chanID = cmd->chanID;
	msg.cmdID = cmd->cmdID;
	msg.cmdID_valid = cmd->cmdID_valid;
	msg.is_error = 1;
	msg.pkt_size = pkt_size;
	msg.total_size = error_val;

	sphcs_msg_scheduler_queue_add_msg(g_the_sphcs->public_respq, &msg.value, 1);
}

struct error_list_work {
	struct work_struct           work;
	struct inf_context          *context;
	union h2c_ExecErrorList      cmd;
	struct inf_cmd_list         *cmdlist;
	void                        *err_buffer;
	uint16_t                     err_buffer_size;
	uint32_t                     total_sent;
	page_handle                  dma_page_hndl;
	dma_addr_t                   dma_page_addr;
	void                        *dma_page_vptr;
	uint32_t                     curr_xfer_size;
};

static int error_list_send_next_packet(struct error_list_work *op);

static int error_list_dma_completed(struct sphcs *sphcs, void *ctx, const void *user_data, int status, u32 timeUS)
{
	struct error_list_work *op = (struct error_list_work *)ctx;
	union c2h_ExecErrorList reply;
	int ret;

	reply.value = 0;
	reply.opcode = SPH_IPC_C2H_OP_CHAN_EXEC_ERROR_LIST;
	reply.chanID = op->cmd.chanID;
	reply.cmdID = op->cmd.cmdID;
	reply.cmdID_valid = op->cmd.cmdID_valid;
	reply.pkt_size = (op->curr_xfer_size - 1);

	if (status == SPHCS_DMA_STATUS_FAILED) {
		reply.is_error = 1;
		reply.total_size = SPH_IPC_DMA_ERROR;
	} else {
		reply.total_size = op->err_buffer_size;
	}

	sphcs_msg_scheduler_queue_add_msg(g_the_sphcs->public_respq, &reply.value, 1);

	if (!reply.is_error && op->total_sent < op->err_buffer_size) {
		op->curr_xfer_size = 0;
		ret = error_list_send_next_packet(op);
		if (ret == 0)
			return 0;
	}

	dma_page_pool_set_page_free(sphcs->dma_page_pool, op->dma_page_hndl);
	if (op->cmdlist != NULL)
		inf_cmd_put(op->cmdlist);
	inf_context_put(op->context);
	kfree(op);

	return 0;
}

static int error_list_send_next_packet(struct error_list_work *op)
{
	dma_addr_t host_dma_page_addr;
	struct sphcs_host_rb *resp_data_rb = &op->context->chan->c2h_rb[0];
	uint32_t chunk_size;
	enum event_val err_val;
	int n;
	int ret;

	if (op->total_sent >= op->err_buffer_size)
		return -EINVAL;

	n = host_rb_wait_free_space(resp_data_rb,
				    SPH_PAGE_SIZE,
				    1,
				    &host_dma_page_addr,
				    &chunk_size);
	if (n != 1 || chunk_size != SPH_PAGE_SIZE) {
		err_val = SPH_IPC_NOT_SUPPORTED;
		goto send_error;
	}
	host_rb_update_free_space(resp_data_rb, SPH_PAGE_SIZE);

	op->curr_xfer_size = op->err_buffer_size - op->total_sent;
	if (op->curr_xfer_size > SPH_PAGE_SIZE)
		op->curr_xfer_size = SPH_PAGE_SIZE;

	memcpy(op->dma_page_vptr,
	       (void *)((uintptr_t)op->err_buffer + op->total_sent),
	       op->curr_xfer_size);

	op->total_sent += op->curr_xfer_size;

	ret = sphcs_dma_sched_start_xfer_single(g_the_sphcs->dmaSched,
						&op->context->chan->c2h_dma_desc,
						op->dma_page_addr,
						host_dma_page_addr,
						op->curr_xfer_size,
						error_list_dma_completed,
						op,
						NULL, 0);
	if (ret != 0) {
		err_val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	return 0;

send_error:
	send_exec_error_cmd_error(&op->cmd, err_val, op->curr_xfer_size);
	return -1;
}

static void error_list_work_handler(struct work_struct *work)
{
	struct error_list_work *op = container_of(work,
						  struct error_list_work,
						  work);
	enum event_val err_val;
	int ret;

	if (op->cmd.cmdID_valid) {
		op->cmdlist = inf_context_find_cmd(op->context, op->cmd.cmdID);
		if (unlikely(op->cmdlist == NULL)) {
			err_val = SPH_IPC_NO_SUCH_CMD;
			goto send_error;
		}
		inf_cmd_get(op->cmdlist);
	} else
		op->cmdlist = NULL;

	if (op->cmd.clear) {
		if (op->cmdlist == NULL) {
			if (op->context->destroyed != 0) {
				err_val = SPH_IPC_NO_SUCH_CONTEXT;
				goto send_error;
			}

			switch (inf_context_get_state(op->context)) {
			case CONTEXT_BROKEN_NON_RECOVERABLE:
				err_val = SPH_IPC_CONTEXT_BROKEN;
				goto send_error;
			case CONTEXT_OK:
				sph_log_info(CREATE_COMMAND_LOG, "Got request to recover non-broken context: 0x%x\n",
					     op->context->protocolID);
			default:
				break;
			}
		} else if (op->cmdlist->destroyed != 0) {
			err_val = SPH_IPC_NO_SUCH_CMD;
			goto send_error;
		}

		inf_exec_error_list_clear(op->cmdlist != NULL ? &op->cmdlist->error_list :
								&op->context->error_list,
					  op->cmdlist);
		goto done;
	}

	ret = inf_exec_error_list_buffer_pack(op->cmdlist != NULL ? &op->cmdlist->error_list :
								    &op->context->error_list,
					      &op->err_buffer,
					      &op->err_buffer_size);

	if (ret != 0) {
		switch (ret) {
		case -ENOENT:
			err_val = SPH_IPC_NO_EXEC_ERRORS;
			break;

		case -ENOSPC:
		case -ENOMEM:
		default:
			err_val = SPH_IPC_NO_MEMORY;
		}
		goto send_error;
	}

	ret = dma_page_pool_get_free_page(g_the_sphcs->dma_page_pool,
					  &op->dma_page_hndl,
					  &op->dma_page_vptr,
					  &op->dma_page_addr);
	if (unlikely(ret < 0)) {
		err_val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	op->total_sent = 0;
	op->curr_xfer_size = 0;
	error_list_send_next_packet(op);
	return;

send_error:
	send_exec_error_cmd_error(&op->cmd, err_val, 0);
done:
	if (op->cmdlist != NULL)
		inf_cmd_put(op->cmdlist);
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(CHAN_EXEC_ERROR_LIST)(struct sphcs              *sphcs,
					      union h2c_ExecErrorList   *cmd)
{
	struct error_list_work *work;
	struct inf_context *context;
	enum event_val error_val;

	context = find_and_get_context(g_the_sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL)) {
		error_val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(context->chan == NULL || context->chan->protocolID != cmd->chanID)) {
		inf_context_put(context);
		error_val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		error_val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	work->cmd.value = cmd->value;
	work->context = context;
	INIT_WORK(&work->work, error_list_work_handler);
	queue_work(context->chan->wq, &work->work);

	return;

send_error:
	send_exec_error_cmd_error(cmd, error_val, 0);
}

void IPC_OPCODE_HANDLER(SCHEDULE_COPY)(struct sphcs                 *sphcs,
				       union h2c_InferenceSchedCopy *cmd)
{
	struct inf_context *context;
	struct inf_copy *copy;
	struct inf_exec_req *req;
	enum event_val val = SPH_IPC_NO_MEMORY;
	int ret;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		val = SPH_IPC_CONTEXT_BROKEN;
		goto put_ctx;
	}

	if (unlikely(context->num_optimized_cmd_lists > 0)) {
		val = SPH_IPC_NOT_SUPPORTED;
		goto put_ctx;
	}

	copy = inf_context_find_and_get_copy(context, cmd->protCopyID);
	if (unlikely(copy == NULL)) {
		val = SPH_IPC_NO_SUCH_COPY;
		goto put_ctx;
	}

	req = kmem_cache_alloc(copy->context->exec_req_slab_cache, GFP_NOWAIT);
	if (unlikely(req == NULL))
		goto put_copy;

	inf_copy_req_init(req, copy, NULL, cmd->copySize, cmd->priority);

	ret = req->f->schedule(req);
	if (unlikely(ret < 0)) {
		kmem_cache_free(copy->context->exec_req_slab_cache, req);
		goto put_copy;
	}

	inf_copy_put(copy);
	inf_context_put(context);

	return;

put_copy:
	inf_copy_put(copy);
put_ctx:
	inf_context_put(context);
send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_COPY_FAILED, val,
				cmd->ctxID, cmd->protCopyID);
}

void IPC_OPCODE_HANDLER(CHAN_SCHEDULE_COPY)(struct sphcs                 *sphcs,
					    union h2c_ChanInferenceSchedCopy *cmd)
{
	struct inf_context *context;
	struct inf_copy *copy;
	struct inf_exec_req *req;
	enum event_val val;
	int ret;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(context->chan == NULL || context->chan->protocolID != cmd->chanID)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto put_ctx;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		val = SPH_IPC_CONTEXT_BROKEN;
		goto put_ctx;
	}

	if (unlikely(context->num_optimized_cmd_lists > 0)) {
		val = SPH_IPC_NOT_SUPPORTED;
		goto put_ctx;
	}

	copy = inf_context_find_and_get_copy(context, cmd->protCopyID);
	if (unlikely(copy == NULL)) {
		val = SPH_IPC_NO_SUCH_COPY;
		goto put_ctx;
	}

	req = kmem_cache_alloc(copy->context->exec_req_slab_cache, GFP_NOWAIT);
	if (unlikely(req == NULL)) {
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	inf_copy_req_init(req, copy, NULL, cmd->copySize, cmd->priority);

	ret = req->f->schedule(req);
	if (unlikely(ret < 0)) {
		kmem_cache_free(copy->context->exec_req_slab_cache, req);
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	inf_copy_put(copy);
	inf_context_put(context);

	return;

put_copy:
	inf_copy_put(copy);
put_ctx:
	inf_context_put(context);
send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_COPY_FAILED, val,
				cmd->chanID, cmd->protCopyID);
}

void IPC_OPCODE_HANDLER(CHAN_SCHEDULE_COPY_LARGE)(struct sphcs                 *sphcs,
						  union h2c_ChanInferenceSchedCopyLarge *cmd)
{
	struct inf_context *context;
	struct inf_copy *copy;
	struct inf_exec_req *req;
	enum event_val val;
	int ret;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(context->chan == NULL || context->chan->protocolID != cmd->chanID)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto put_ctx;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		val = SPH_IPC_CONTEXT_BROKEN;
		goto put_ctx;
	}

	if (unlikely(context->num_optimized_cmd_lists > 0)) {
		val = SPH_IPC_NOT_SUPPORTED;
		goto put_ctx;
	}

	copy = inf_context_find_and_get_copy(context, cmd->protCopyID);
	if (unlikely(copy == NULL)) {
		val = SPH_IPC_NO_SUCH_COPY;
		goto put_ctx;
	}

	req = kmem_cache_alloc(copy->context->exec_req_slab_cache, GFP_NOWAIT);
	if (unlikely(req == NULL)) {
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	inf_copy_req_init(req, copy, NULL, cmd->copySize, cmd->priority);

	ret = req->f->schedule(req);
	if (unlikely(ret < 0)) {
		kmem_cache_free(copy->context->exec_req_slab_cache, req);
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	inf_copy_put(copy);
	inf_context_put(context);

	return;

put_copy:
	inf_copy_put(copy);
put_ctx:
	inf_context_put(context);
send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_COPY_FAILED, val,
				cmd->chanID, cmd->protCopyID);
}

void IPC_OPCODE_HANDLER(CHAN_SCHEDULE_COPY_SUBRES)(struct sphcs                 *sphcs,
						   union h2c_ChanInferenceSchedCopySubres *cmd)
{
	struct inf_context *context;
	struct inf_copy *copy;
	struct inf_exec_req *req;
	enum event_val val;
	int ret;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(context->chan == NULL || context->chan->protocolID != cmd->chanID)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto put_ctx;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		val = SPH_IPC_CONTEXT_BROKEN;
		goto put_ctx;
	}

	copy = inf_context_find_and_get_copy(context, cmd->protCopyID);
	if (unlikely(copy == NULL)) {
		val = SPH_IPC_NO_SUCH_COPY;
		goto put_ctx;
	}

	req = kmem_cache_alloc(copy->context->exec_req_slab_cache, GFP_NOWAIT);
	if (unlikely(req == NULL)) {
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	ret = inf_copy_req_init_subres_copy(req,
					    copy,
					    cmd->hostresID,
					    cmd->dstOffset,
					    cmd->copySize);
	if (ret) {
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	ret = req->f->schedule(req);
	if (unlikely(ret < 0)) {
		kmem_cache_free(copy->context->exec_req_slab_cache, req);
		val = SPH_IPC_NO_MEMORY;
		goto put_copy;
	}

	inf_copy_put(copy);
	inf_context_put(context);

	return;

put_copy:
	inf_copy_put(copy);
put_ctx:
	inf_context_put(context);
send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_COPY_FAILED, val,
				cmd->chanID, cmd->protCopyID);
}

static void handle_sched_cmdlist(struct inf_cmd_list *cmdlist,
				 struct req_params   *params,
				 uint16_t             num_params)
{
	struct inf_context  *context = cmdlist->context;
	struct inf_exec_req *req;
	unsigned long flags;
	int ret = 0;
	uint16_t i, k, j = 0;

	SPH_ASSERT(params != NULL || num_params == 0);

	SPH_SPIN_LOCK_IRQSAVE(&cmdlist->lock_irq, flags);
	SPH_ASSERT(cmdlist->num_left == 0);
	++cmdlist->num_left; // to prevent completion reports in time of schedule
	SPH_SPIN_UNLOCK_IRQRESTORE(&cmdlist->lock_irq, flags);

	// for schedule
	inf_cmd_get(cmdlist);

	DO_TRACE(trace_cmdlist(SPH_TRACE_OP_STATUS_START, context->protocolID, cmdlist->protocolID));

	for (i = 0; i < cmdlist->num_reqs; ++i) {
		req = kmem_cache_alloc(context->exec_req_slab_cache, GFP_NOWAIT);
		if (unlikely(req == NULL))
			break;

		memcpy(req, &cmdlist->req_list[i], sizeof(struct inf_exec_req));

		k = 0;
		for ( ; j < num_params && i == params[j].idx; ++j) {
			switch (req->cmd_type) {
			case CMDLIST_CMD_COPYLIST:
				SPH_ASSERT(params[j].cpy_idx < req->cpylst->n_copies);
				/* recompute priority */
				if (k == 0)
					req->priority = 0;
				for ( ; req->priority == 0 && k < params[j].cpy_idx; ++k)
					if (req->cpylst->priorities[k] == 1)
						req->priority = 1;
				if (req->priority == 0 && params[j].priority == 1) {
					req->priority = 1;
					++k;
				}
				req->cpylst->cur_sizes[params[j].cpy_idx] = params[j].size;
				req->size -= req->cpylst->sizes[params[j].cpy_idx];
				req->size += params[j].size;
				break;
			case CMDLIST_CMD_COPY:
				req->priority = params[j].priority;
				req->size = params[j].size;
				break;
			case CMDLIST_CMD_INFREQ:
				req->sched_params_is_null = params[j].sched_params_is_null;
				if (!req->sched_params_is_null) {
					req->priority = params[j].priority;
					req->size = params[j].size;
					req->debugOn = params[j].debugOn;
					req->collectInfo = params[j].collectInfo;
				}
				break;
			};
		}
		if (k != 0) { //cpylist params were overwritten
			for ( ; req->priority == 0 && k < req->cpylst->n_copies; ++k) {
				if (req->cpylst->priorities[k] == 1)
					req->priority = 1;
			}
			inf_cpylst_build_cur_lli(req->cpylst);
			req->lli_addr = req->cpylst->cur_lli_addr;
		}

		SPH_SPIN_LOCK_IRQSAVE(&cmdlist->lock_irq, flags);
		++cmdlist->num_left;
		SPH_SPIN_UNLOCK_IRQRESTORE(&cmdlist->lock_irq, flags);

		ret = req->f->schedule(req);
		if (unlikely(ret < 0)) {
			SPH_SPIN_LOCK_IRQSAVE(&cmdlist->lock_irq, flags);
			--cmdlist->num_left;
			SPH_SPIN_UNLOCK_IRQRESTORE(&cmdlist->lock_irq, flags);
			kmem_cache_free(context->exec_req_slab_cache, req);
			break;
		}
	}
	if (unlikely(i < cmdlist->num_reqs)) {
		for ( ; i < cmdlist->num_reqs; ++i)
			cmdlist->req_list[i].f->send_report(&cmdlist->req_list[i], SPH_IPC_NO_MEMORY);
	}
	SPH_SPIN_LOCK_IRQSAVE(&cmdlist->lock_irq, flags);
	if (--cmdlist->num_left == 0) {// schedule finished
		SPH_SPIN_UNLOCK_IRQRESTORE(&cmdlist->lock_irq, flags);
		goto send_completion;
	}
	SPH_SPIN_UNLOCK_IRQRESTORE(&cmdlist->lock_irq, flags);

	goto done;

send_completion:
	// for schedule
	inf_cmd_put(cmdlist);
	sphcs_send_event_report(g_the_sphcs, SPH_IPC_EXECUTE_CMD_COMPLETE, 0,
				context->protocolID, cmdlist->protocolID);
done:
	cmdlist->edits_idx = 0;
}

static int cmdlist_schedule_dma_complete(struct sphcs *sphcs,
					 void *ctx,
					 const void *user_data,
					 int status,
					 u32 xferTimeUS)
{
	struct inf_cmd_list *cmd = (struct inf_cmd_list *)ctx;
	uint32_t num_edits, cmd_index;
	uint8_t byte;
	uint16_t i, protID, ncopies;
	uint8_t sched_params_are_null;
	uint8_t *begin, *p = cmd->vptr;
	uint16_t sched_dma_size;
	bool is_last;
	enum event_val val;
	int ret = 0;

	if (cmd->context->chan != NULL) {
		union h2c_ChanInferenceCmdListOp *command = (union h2c_ChanInferenceCmdListOp *)user_data;

		sched_dma_size = command->size;
		is_last = command->is_last;

		sphcs_cmd_chan_update_cmd_head(cmd->context->chan, 0, PAGE_SIZE);
	} else {//UMD1 path
		union h2c_InferenceCmdListOp *command = (union h2c_InferenceCmdListOp *)user_data;
		union c2h_DmaPageHandleFree free_page_msg = { .value = 0 };

		sched_dma_size = command->size;
		is_last = command->is_last;

		free_page_msg.opcode = SPH_IPC_C2H_OP_DMA_PAGE_HANDLE_FREE;
		free_page_msg.host_page_hndl = command->host_page_hndl;
		ret = sphcs_msg_scheduler_queue_add_msg(g_the_sphcs->public_respq, &free_page_msg.value, 1);
		if (unlikely(ret < 0)) {
			val = SPH_IPC_NO_MEMORY;
			goto finish;
		}
	}

	if (unlikely(status == SPHCS_DMA_STATUS_FAILED)) {
		val = SPH_IPC_DMA_ERROR;
		ret = -EFAULT;
		goto finish;
	}

	if (cmd->edits_idx == 0 && cmd->num_left == 0) { //First DMA
		POP_VALUE(p, uint32_t, &num_edits);//We don't need it, "edits" is allocated on create
		sched_dma_size -= sizeof(uint32_t);
		if (unlikely(num_edits == 0)) {
			val = SPH_IPC_RUNTIME_NOT_SUPPORTED;
			ret = -EINVAL;
			goto finish;
		}
	}

	while (sched_dma_size > 0) {
		begin = p;
		POP_VALUE(p, uint32_t, &cmd_index);
		POP_VALUE(p, uint8_t, &byte);
		switch (byte) {
		case CMDLIST_CMD_COPY:
			POP_VALUE(p, uint16_t, &protID); //not used here
			POP_VALUE(p, uint8_t, &cmd->edits[cmd->edits_idx].priority);
			POP_VALUE(p, uint64_t, &cmd->edits[cmd->edits_idx].size);
			if (cmd->num_left == 0) {// standalone copy
				SPH_ASSERT(cmd->req_list[cmd_index].cmd_type == CMDLIST_CMD_COPY);
				cmd->edits[cmd->edits_idx].idx = cmd_index;
			} else { // copy in cpylst
				SPH_ASSERT(cmd->req_list[cmd->edits[cmd->edits_idx].idx].cmd_type == CMDLIST_CMD_COPYLIST);
				// cmd->edits[cmd->edits_idx].idx is already uptodate
				cmd->edits[cmd->edits_idx].cpy_idx = cmd_index;
				--cmd->num_left;
			}
			++cmd->edits_idx;
			break;
		case CMDLIST_CMD_INFREQ:
			SPH_ASSERT(cmd->req_list[cmd_index].cmd_type == CMDLIST_CMD_INFREQ);
			SPH_ASSERT(cmd->num_left == 0);
			POP_VALUE(p, uint16_t, &protID); //not used here
			POP_VALUE(p, uint16_t, &protID); //not used here
			cmd->edits[cmd->edits_idx].idx = cmd_index;
			POP_VALUE(p, uint8_t, &sched_params_are_null);
			cmd->req_list[cmd_index].sched_params_is_null = sched_params_are_null;
			if (sched_params_are_null == 0) {
				POP_VALUE(p, uint16_t, &cmd->req_list[cmd_index].size);
				POP_VALUE(p, uint8_t, &cmd->req_list[cmd_index].priority);
				POP_VALUE(p, uint8_t, &byte); cmd->req_list[cmd_index].debugOn = byte;
				POP_VALUE(p, uint8_t, &byte); cmd->req_list[cmd_index].collectInfo = byte;
			}
			++cmd->edits_idx;
			break;
		case CMDLIST_CMD_COPYLIST:
			SPH_ASSERT(cmd->req_list[cmd_index].cmd_type == CMDLIST_CMD_COPYLIST);
			SPH_ASSERT(cmd->num_left == 0);
			POP_VALUE(p, uint16_t, &ncopies);
			SPH_ASSERT(ncopies > 0);
			for (i = cmd->edits_idx; i < cmd->edits_idx + ncopies; ++i)
				cmd->edits[i].idx = cmd_index;
			cmd->num_left = ncopies;
			break;
		default:
			//NOT supported
			val = SPH_IPC_RUNTIME_NOT_SUPPORTED;
			ret = -EINVAL;
			goto finish;
		}
		sched_dma_size -= ((uint8_t *)p - begin);
	}

	// Do not start schedule if not last packet
	if (!is_last)
		goto finish;

	SPH_ASSERT(cmd->num_left == 0);

	handle_sched_cmdlist(cmd, cmd->edits, cmd->edits_idx);
	cmd->edits_idx = 0;

finish:
	// put kref for DMA
	inf_cmd_put(cmd);

	if (unlikely(ret < 0)) {
		cmd->edits_idx = 0;
		cmd->num_left = 0;
		sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_EXECUTE_CMD_COMPLETE,
					val,
					cmd->context->protocolID,
					cmd->protocolID);
	}

	return ret;
}

void IPC_OPCODE_HANDLER(SCHEDULE_CMDLIST)(struct sphcs                 *sphcs,
					  union h2c_InferenceCmdListOp *cmd)
{
	struct inf_context *context;
	struct inf_cmd_list *cmdlist;
	enum event_val val = 0;
	int ret;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		inf_context_put(context);
		val = SPH_IPC_CONTEXT_BROKEN;
		goto send_error;
	}

	cmdlist = inf_context_find_cmd(context, cmd->cmdID);
	if (unlikely(cmdlist == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_SUCH_CMD;
		goto send_error;
	}
	SPH_ASSERT(cmdlist->status == CREATED);
	inf_context_put(context);

	if (cmd->is_first == 0 || cmd->is_last == 0) { //TODO Add support to overwrite parameters for more than 1 page
		val = SPH_IPC_RUNTIME_NOT_SUPPORTED;
		goto send_error;
	}
	if (cmd->is_first) {
		SPH_ASSERT(cmdlist->edits_idx == 0);
		SPH_ASSERT(cmdlist->num_left == 0);
		DO_TRACE(trace_cmdlist(SPH_TRACE_OP_STATUS_QUEUED, cmd->ctxID, cmd->cmdID));
		if (cmd->size == 0) { //No overwritten parameters
			SPH_ASSERT(cmd->is_last == 1);
			handle_sched_cmdlist(cmdlist, NULL, 0);

			return;
		}
	}

	SPH_ASSERT(cmd->size > 0);

	// for DMA
	inf_cmd_get(cmdlist);

	ret = sphcs_dma_sched_start_xfer_single(g_the_sphcs->dmaSched,
						&cmdlist->h2c_dma_desc,//TODO: consider use high priority
						SPH_IPC_DMA_PFN_TO_ADDR(cmd->host_pfn),
						cmdlist->dma_addr,
						cmd->size,
						cmdlist_schedule_dma_complete,
						cmdlist,
						cmd,
						sizeof(*cmd));

	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto fail;
	}

	return;

fail:
	// put kref for DMA
	inf_cmd_put(cmdlist);
send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_CMD_COMPLETE, val,
				cmd->ctxID, cmd->cmdID);
}

void IPC_OPCODE_HANDLER(SCHEDULE_CMDLIST_NO_OW)(struct sphcs                    *sphcs,
						union h2c_InferenceSchedCmdList *cmd)
{
	struct inf_context *context;
	struct inf_cmd_list *cmdlist;
	enum event_val val = 0;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		inf_context_put(context);
		val = SPH_IPC_CONTEXT_BROKEN;
		goto send_error;
	}

	cmdlist = inf_context_find_cmd(context, cmd->cmdID);
	if (unlikely(cmdlist == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_SUCH_CMD;
		goto send_error;
	}
	SPH_ASSERT(cmdlist->status == CREATED);
	inf_context_put(context);


	SPH_ASSERT(cmdlist->edits_idx == 0);
	SPH_ASSERT(cmdlist->num_left == 0);
	DO_TRACE(trace_cmdlist(SPH_TRACE_OP_STATUS_QUEUED, cmd->ctxID, cmd->cmdID));
	handle_sched_cmdlist(cmdlist, NULL, 0);

	return;

send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_CMD_COMPLETE, val,
				cmd->ctxID, cmd->cmdID);
}

void IPC_OPCODE_HANDLER(CHAN_SCHEDULE_CMDLIST)(struct sphcs                    *sphcs,
					       union h2c_ChanInferenceCmdListOp *cmd)
{
	struct inf_context *context;
	struct inf_cmd_list *cmdlist;
	enum event_val val = 0;
	dma_addr_t host_dma_addr;
	struct sphcs_host_rb *cmd_data_rb;
	u32 host_chunk_size;
	int ret;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (cmd->size > 0) {
		cmd_data_rb = &context->chan->h2c_rb[0];
		/* need to advance h2c ring buffer by one page */
		host_rb_update_free_space(cmd_data_rb, SPH_PAGE_SIZE);
		ret = host_rb_get_avail_space(cmd_data_rb,
					      SPH_PAGE_SIZE,
					      1,
					      &host_dma_addr,
					      &host_chunk_size);

		SPH_ASSERT(ret == 1);
		SPH_ASSERT((host_dma_addr & SPH_IPC_DMA_ADDR_ALIGN_MASK) == 0);

		host_rb_update_avail_space(cmd_data_rb, SPH_PAGE_SIZE);
	}


	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		inf_context_put(context);
		val = SPH_IPC_CONTEXT_BROKEN;
		goto send_error;
	}

	cmdlist = inf_context_find_cmd(context, cmd->cmdID);
	if (unlikely(cmdlist == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_SUCH_CMD;
		goto send_error;
	}
	SPH_ASSERT(cmdlist->status == CREATED);
	inf_context_put(context);

	if (cmd->is_first == 0 || cmd->is_last == 0) { //TODO Add support to overwrite parameters for more than 1 page
		val = SPH_IPC_RUNTIME_NOT_SUPPORTED;
		goto send_error;
	}
	if (cmd->is_first) {
		SPH_ASSERT(cmdlist->edits_idx == 0);
		SPH_ASSERT(cmdlist->num_left == 0);
		DO_TRACE(trace_cmdlist(SPH_TRACE_OP_STATUS_QUEUED, cmdlist->context->protocolID, cmdlist->protocolID));
		if (cmd->size == 0) { //No overwritten parameters
			SPH_ASSERT(cmd->is_last == 1);
			handle_sched_cmdlist(cmdlist, NULL, 0);

			return;
		}
	}

	SPH_ASSERT(cmd->size > 0);

	// for DMA
	inf_cmd_get(cmdlist);


	ret = sphcs_dma_sched_start_xfer_single(g_the_sphcs->dmaSched,
						&context->chan->h2c_dma_desc,//TODO: consider use high priority
						host_dma_addr,
						cmdlist->dma_addr,
						cmd->size,
						cmdlist_schedule_dma_complete,
						cmdlist,
						cmd,
						sizeof(*cmd));

	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto fail;
	}

	return;

fail:
	// put kref for DMA
	inf_cmd_put(cmdlist);
send_error:
	if (context != NULL)
		sphcs_cmd_chan_update_cmd_head(context->chan, 0, PAGE_SIZE);
	sphcs_send_event_report(sphcs, SPH_IPC_EXECUTE_CMD_COMPLETE, val,
				cmd->chanID, cmd->cmdID);
}


struct subres_load_op_work {
	struct work_struct           work;
	struct inf_context          *context;
	union h2c_SubResourceLoadOp    cmd;
};

static void subres_load_op_work_handler(struct work_struct *work)
{
	struct subres_load_op_work *op = container_of(work,
					       struct subres_load_op_work,
					       work);
	enum event_val val;

	val = inf_subresload_execute(op->context, &op->cmd);
	if (unlikely(val != 0)) {
		sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_ERROR_SUB_RESOURCE_LOAD_FAILED,
					val,
					op->cmd.contextID,
					op->cmd.sessionID);
		goto free_op;
	}

free_op:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_SUBRES_LOAD)(struct sphcs *sphcs,
					 union h2c_SubResourceLoadOp *cmd)
{
	struct subres_load_op_work *work;
	struct inf_context *context;
	enum event_val val;

	context = find_and_get_context(sphcs->inf_data, cmd->contextID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(&work->cmd, cmd, sizeof(*cmd));
	work->context = context;
	INIT_WORK(&work->work, subres_load_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE(trace_inf_net_subres(cmd->contextID, cmd->sessionID, cmd->res_offset, cmd->host_pool_index, -1, -1, SPH_TRACE_OP_STATUS_QUEUED));

	return;

send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_ERROR_SUB_RESOURCE_LOAD_FAILED,
				val, cmd->contextID, cmd->sessionID);
}

struct subres_load_create_session_op_work {
	struct work_struct           work;
	struct inf_context          *context;
	union h2c_SubResourceLoadCreateRemoveSession    cmd;
};

static void subres_load_create_session_op_work_handler(struct work_struct *work)
{
	int ret = 0;
	struct subres_load_create_session_op_work *op = container_of(work,
					       struct subres_load_create_session_op_work,
					       work);
	struct inf_devres *devres;
	enum event_val val;

	if (op->cmd.remove == 1) {
		inf_context_remove_subres_load_session(op->context,
						       op->cmd.sessionID);
		goto done;
	}

	devres = inf_context_find_devres(op->context, op->cmd.resID);
	if (unlikely(devres == NULL)) {
		val = SPH_IPC_NO_SUCH_DEVRES;
		goto send_error;
	}

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_NET_SUBRES_CREATE_SESSION, op->cmd.contextID,
		op->cmd.sessionID, SPH_TRACE_OP_STATUS_START, op->cmd.resID, -1));

	ret = inf_subresload_create_session(op->context, devres, &op->cmd);
	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_NET_SUBRES_CREATE_SESSION, op->cmd.contextID,
		op->cmd.sessionID, SPH_TRACE_OP_STATUS_COMPLETE, op->cmd.resID, -1));

	goto done;

send_error:
	sphcs_send_event_report(g_the_sphcs, SPH_IPC_ERROR_SUB_RESOURCE_LOAD_FAILED,
				val, op->cmd.contextID, op->cmd.resID);
done:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_SUBRES_LOAD_CREATE_REMOVE_SESSION)(struct sphcs                  *sphcs,
							      union h2c_SubResourceLoadCreateRemoveSession     *cmd)
{
	struct subres_load_create_session_op_work *work;
	struct inf_context *context;
	enum event_val val;

	context = find_and_get_context(sphcs->inf_data, cmd->contextID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(&work->cmd, cmd, sizeof(*cmd));
	work->context = context;
	INIT_WORK(&work->work, subres_load_create_session_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE_IF(!cmd->remove, trace_infer_create(SPH_TRACE_INF_NET_SUBRES_CREATE_SESSION,
			cmd->contextID, cmd->sessionID, SPH_TRACE_OP_STATUS_QUEUED, cmd->resID, -1));

	return;

send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_ERROR_SUB_RESOURCE_LOAD_FAILED,
				val, cmd->contextID, cmd->resID);
}

struct inf_req_op_work {
	struct work_struct           work;
	struct inf_context          *context;
	union  h2c_InferenceReqOp    cmd;
	int rbID;
};

static void inf_req_op_work_handler(struct work_struct *work)
{
	struct inf_req_op_work *op = container_of(work,
						  struct inf_req_op_work,
						  work);
	struct inf_devnet *devnet;
	struct inf_req *infreq;
	uint8_t event;
	enum event_val val;
	dma_addr_t host_dma_addr;
	int ret;

	if (op->cmd.destroy)
		event = SPH_IPC_DESTROY_INFREQ_FAILED;
	else
		event = SPH_IPC_CREATE_INFREQ_FAILED;

	if (!op->cmd.destroy) {
		if (op->context->chan != NULL) {
			struct sphcs_host_rb *cmd_data_rb = &op->context->chan->h2c_rb[op->rbID];
			u32 host_chunk_size;
			int n;

			/* need to advance h2c ring buffer by one page */
			host_rb_update_free_space(cmd_data_rb, SPH_PAGE_SIZE);
			n = host_rb_get_avail_space(cmd_data_rb,
						    SPH_PAGE_SIZE,
						    1,
						    &host_dma_addr,
						    &host_chunk_size);

			SPH_ASSERT(n == 1);
			SPH_ASSERT((host_dma_addr & SPH_IPC_DMA_ADDR_ALIGN_MASK) == 0);

			host_rb_update_avail_space(cmd_data_rb, SPH_PAGE_SIZE);
		} else {
			host_dma_addr = SPH_IPC_DMA_PFN_TO_ADDR(op->cmd.host_pfn);
		}

		if (!check_memory_threshold()) {
			val = SPH_IPC_NO_MEMORY;
			goto send_error;
		}
	}

	devnet = inf_context_find_and_get_devnet(op->context, op->cmd.netID, false, true);
	if (unlikely(devnet == NULL)) {
		val = SPH_IPC_NO_SUCH_NET;
		goto send_error;
	}

	if (op->cmd.destroy) {
		ret = inf_devnet_find_and_destroy_infreq(devnet, op->cmd.infreqID);
		if (unlikely(ret < 0)) {
			val = SPH_IPC_NO_SUCH_INFREQ;
			goto error_put_devnet;
		}
		inf_devnet_put(devnet);
		goto done;
	}

	if (unlikely(devnet->destroyed)) {
		val = SPH_IPC_NO_SUCH_NET;
		goto error_put_devnet;
	}

	infreq = inf_devnet_find_infreq(devnet, op->cmd.infreqID);
	if (unlikely(infreq != NULL)) {
		val = SPH_IPC_ALREADY_EXIST;
		goto error_put_devnet;
	}

	DO_TRACE(trace_infer_create(SPH_TRACE_INF_CREATE_INF_REQ, op->cmd.ctxID, op->cmd.infreqID, SPH_TRACE_OP_STATUS_START, -1, -1));

	if (unlikely(op->cmd.size > SPH_PAGE_SIZE)) {
		val = SPH_IPC_DMA_ERROR;
		goto error_put_devnet;
	}


	ret = inf_devnet_create_infreq(devnet,
				       op->cmd.infreqID,
				       host_dma_addr,
				       op->cmd.host_page_hndl,
				       op->cmd.size);
	if (unlikely(ret < 0)) {
		val = SPH_IPC_NO_MEMORY;
		goto error_put_devnet;
	}

	inf_devnet_put(devnet);
	goto done;

error_put_devnet:
	inf_devnet_put(devnet);
send_error:
	if (!op->cmd.destroy && op->context->chan != NULL)
		sphcs_cmd_chan_update_cmd_head(op->context->chan, 0, SPH_PAGE_SIZE);
	sphcs_send_event_report_ext(g_the_sphcs, event, val,
				op->cmd.ctxID,
				op->cmd.infreqID,
				op->cmd.netID);
done:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_REQ_OP)(struct sphcs             *sphcs,
				    union h2c_InferenceReqOp *cmd)
{
	struct inf_req_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_INFREQ_FAILED;
	else
		event = SPH_IPC_CREATE_INFREQ_FAILED;

	context = find_and_get_context(g_the_sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(&work->cmd, cmd, sizeof(*cmd));
	work->context = context;
	INIT_WORK(&work->work, inf_req_op_work_handler);
	queue_work(context->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy, trace_infer_create(SPH_TRACE_INF_CREATE_INF_REQ, cmd->ctxID, cmd->infreqID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report_ext(sphcs, event, val,
				    cmd->ctxID, cmd->infreqID, cmd->netID);
}

void IPC_OPCODE_HANDLER(CHAN_INF_REQ_OP)(struct sphcs             *sphcs,
					 union h2c_ChanInferenceReqOp *cmd)
{
	struct inf_req_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->destroy)
		event = SPH_IPC_DESTROY_INFREQ_FAILED;
	else
		event = SPH_IPC_CREATE_INFREQ_FAILED;

	context = find_and_get_context(g_the_sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memset(&work->cmd, 0, sizeof(*cmd));
	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = cmd->chanID;
	work->cmd.netID = cmd->netID;
	work->cmd.infreqID = cmd->infreqID;
	work->cmd.size = cmd->size;
	work->cmd.destroy = cmd->destroy;
	work->context = context;
	work->rbID = cmd->rbID;
	INIT_WORK(&work->work, inf_req_op_work_handler);
	queue_work(context->chan->wq, &work->work);

	DO_TRACE_IF(!cmd->destroy, trace_infer_create(SPH_TRACE_INF_CREATE_INF_REQ, cmd->chanID, cmd->infreqID, SPH_TRACE_OP_STATUS_QUEUED, -1, -1));

	return;

send_error:
	sphcs_send_event_report_ext(sphcs, event, val,
				    cmd->chanID, cmd->infreqID, cmd->netID);
}

void IPC_OPCODE_HANDLER(SCHEDULE_INF_REQ)(struct sphcs                   *sphcs,
					  union h2c_InferenceReqSchedule *cmd)
{
	struct inf_context *context;
	struct inf_devnet *devnet;
	struct inf_req *infreq;
	struct inf_exec_req *req;
	int ret;
	enum event_val val;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		val = SPH_IPC_CONTEXT_BROKEN;
		goto put_ctx;
	}

	if (unlikely(context->num_optimized_cmd_lists > 0)) {
		val = SPH_IPC_NOT_SUPPORTED;
		goto put_ctx;
	}

	devnet = inf_context_find_and_get_devnet(context, cmd->netID, false, true);
	if (unlikely(devnet == NULL)) {
		val = SPH_IPC_NO_SUCH_NET;
		goto put_ctx;
	}
	inf_context_put(context);

	infreq = inf_devnet_find_and_get_infreq(devnet, cmd->infreqID);
	inf_devnet_put(devnet);
	if (unlikely(infreq == NULL)) {
		val = SPH_IPC_NO_SUCH_INFREQ;
		goto send_error;
	}

	req = kmem_cache_alloc(context->exec_req_slab_cache, GFP_NOWAIT);
	if (unlikely(req == NULL)) {
		inf_req_put(infreq);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	infreq_req_init(req,
			infreq,
			NULL,//cmdlist ptr
			cmd->schedParamsIsNull != 0 ? 0 : cmd->priority,
			cmd->schedParamsIsNull != 0,
			cmd->batchSize,
			cmd->debugOn,
			cmd->collectInfo);

	ret = req->f->schedule(req);
	inf_req_put(infreq);
	if (unlikely(ret < 0)) {
		kmem_cache_free(context->exec_req_slab_cache, req);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	return;

put_ctx:
	inf_context_put(context);
send_error:
	sphcs_send_event_report_ext(sphcs,
				SPH_IPC_SCHEDULE_INFREQ_FAILED,
				val,
				cmd->ctxID,
				cmd->infreqID,
				cmd->netID);
}

void IPC_OPCODE_HANDLER(CHAN_SCHEDULE_INF_REQ)(struct sphcs                   *sphcs,
					       union h2c_ChanInferenceReqSchedule *cmd)
{
	struct inf_context *context;
	struct inf_devnet *devnet;
	struct inf_req *infreq;
	struct inf_exec_req *req;
	int ret;
	enum event_val val;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	if (unlikely(inf_context_get_state(context) != CONTEXT_OK)) {
		val = SPH_IPC_CONTEXT_BROKEN;
		goto put_ctx;
	}

	if (unlikely(context->num_optimized_cmd_lists > 0)) {
		val = SPH_IPC_NOT_SUPPORTED;
		goto put_ctx;
	}

	devnet = inf_context_find_and_get_devnet(context, cmd->netID, false, true);
	if (unlikely(devnet == NULL)) {
		val = SPH_IPC_NO_SUCH_NET;
		goto put_ctx;
	}
	inf_context_put(context);

	infreq = inf_devnet_find_and_get_infreq(devnet, cmd->infreqID);
	inf_devnet_put(devnet);
	if (unlikely(infreq == NULL)) {
		val = SPH_IPC_NO_SUCH_INFREQ;
		goto send_error;
	}

	req = kmem_cache_alloc(context->exec_req_slab_cache, GFP_NOWAIT);
	if (unlikely(req == NULL)) {
		inf_req_put(infreq);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	infreq_req_init(req,
			infreq,
			NULL,//cmdlist ptr
			cmd->schedParamsIsNull != 0 ? 0 : cmd->priority,
			cmd->schedParamsIsNull != 0,
			cmd->batchSize,
			cmd->debugOn,
			cmd->collectInfo);

	ret = req->f->schedule(req);
	inf_req_put(infreq);
	if (unlikely(ret < 0)) {
		kmem_cache_free(context->exec_req_slab_cache, req);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	return;

put_ctx:
	inf_context_put(context);
send_error:
	sphcs_send_event_report_ext(sphcs,
				SPH_IPC_SCHEDULE_INFREQ_FAILED,
				val,
				cmd->chanID,
				cmd->infreqID,
				cmd->netID);
}

struct network_reservation_op_work {
	struct work_struct work;
	struct inf_context *context;
	union h2c_InferenceNetworkResourceReservation cmd;
};

struct network_property_op_work {
	struct work_struct work;
	struct inf_context *context;
	union h2c_InferenceNetworkProperty cmd;
};

static void network_reservation_op_work_handler(struct work_struct *work)
{
	struct network_reservation_op_work	*op = container_of(work,
			struct network_reservation_op_work,
			work);
	struct inf_devnet *devnet;
	struct inf_devnet_resource_reserve cmd_args;
	int ret;
	enum event_val event;

	if (op->cmd.reserve)
		event = SPH_IPC_DEVNET_RESOURCES_RESERVATION_FAILED;
	else
		event = SPH_IPC_DEVNET_RESOURCES_RELEASE_FAILED;

	devnet = inf_context_find_and_get_devnet(op->context, op->cmd.netID, true, true);
	if (unlikely(devnet == NULL)) {
		sphcs_send_event_report(g_the_sphcs,
				event,
				SPH_IPC_NO_SUCH_NET,
				op->cmd.ctxID,
				op->cmd.netID);
		goto free_op;
	}

	/* Network reservation operation cannot run while network editing still in progress */
	SPH_SPIN_LOCK(&devnet->lock);
	if (unlikely(devnet->edit_status != CREATED)) {
		SPH_SPIN_UNLOCK(&devnet->lock);
		inf_devnet_put(devnet);
		sphcs_send_event_report(g_the_sphcs,
				event,
				SPH_IPC_NOT_SUPPORTED,
				op->cmd.ctxID,
				op->cmd.netID);
		goto free_op;
	}
	/* Change edit_status of the network to DMA_COMPLETED
	 * in case runtime crashed before finish handling the network reservation,
	 * we need to be able to release the network ref count acquired.
	 */
	devnet->edit_status = DMA_COMPLETED;
	SPH_SPIN_UNLOCK(&devnet->lock);

	memset(&cmd_args, 0, sizeof(cmd_args));
	cmd_args.devnet_drv_handle = (uint64_t)devnet;
	cmd_args.devnet_rt_handle = (uint64_t)devnet->rt_handle;
	cmd_args.reserve_resource = op->cmd.reserve;
	if (cmd_args.reserve_resource)
		cmd_args.timeout = op->cmd.timeout;

	// kref for RT is taken in find

	ret = inf_cmd_queue_add(&devnet->context->cmdq,
			SPHCS_RUNTIME_CMD_DEVNET_RESOURCES_RESERVATION,
			&cmd_args,
			sizeof(cmd_args),
			NULL, NULL);
	if (unlikely(ret < 0)) {
		sphcs_send_event_report(g_the_sphcs,
				event,
				SPH_IPC_NO_MEMORY,
				op->cmd.ctxID,
				op->cmd.netID);
		inf_devnet_put(devnet);
	}

free_op:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(INF_NETWORK_RESOURCE_RESERVATION)(struct sphcs *sphcs,
		union h2c_InferenceNetworkResourceReservation *cmd) {
	struct network_reservation_op_work *work;
	struct inf_context *context;
	uint8_t event;
	enum event_val val;

	if (cmd->reserve)
		event = SPH_IPC_DEVNET_RESOURCES_RESERVATION_FAILED;
	else
		event = SPH_IPC_DEVNET_RESOURCES_RELEASE_FAILED;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(work->cmd.value, cmd->value, sizeof(work->cmd.value));
	work->context = context;
	INIT_WORK(&work->work, network_reservation_op_work_handler);
	queue_work(context->wq, &work->work);

	return;

send_error:
	sphcs_send_event_report(sphcs, event, val, cmd->ctxID, cmd->netID);
}

static void network_property_op_work_handler(struct work_struct *work)
{
	struct network_property_op_work	*op = container_of(work,
			struct network_property_op_work,
			work);
	struct inf_devnet *devnet;
	enum event_val event_val = 0;

	devnet = inf_context_find_and_get_devnet(op->context, op->cmd.netID, true, true);
	if (unlikely(devnet == NULL)) {
		sphcs_send_event_report(g_the_sphcs,
				SPH_IPC_DEVNET_SET_PROPERTY_FAILED,
				SPH_IPC_NO_SUCH_NET,
				op->cmd.ctxID,
				op->cmd.netID);
		goto free_op;
	}

	switch (op->cmd.property) {
	case SPH_SERIAL_INF_EXECUTION: {
		SPH_SPIN_LOCK(&devnet->lock);
		devnet->serial_infreq_exec = op->cmd.property_val;
		SPH_SPIN_UNLOCK(&devnet->lock);

		inf_devnet_put(devnet);

		sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_DEVNET_SET_PROPERTY_SUCCESS,
					event_val,
					op->cmd.ctxID,
					op->cmd.netID);
		break;
	}
	case SPH_NETWORK_RESOURCES_RESERVATION: {
		struct inf_devnet_resource_reserve cmd_args;
		int ret;

		/* Network reservation operation cannot run while network editing still in progress */
		SPH_SPIN_LOCK(&devnet->lock);
		if (unlikely(devnet->edit_status != CREATED)) {
			SPH_SPIN_UNLOCK(&devnet->lock);
			inf_devnet_put(devnet);
			sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_DEVNET_SET_PROPERTY_FAILED,
					SPH_IPC_NOT_SUPPORTED,
					op->cmd.ctxID,
					op->cmd.netID);
			goto free_op;
		}
		/* Change edit_status of the network to DMA_COMPLETED
		 * in case runtime crashed before finish handling the network reservation,
		 * we need to be able to release the network ref count acquired.
		 */
		devnet->edit_status = DMA_COMPLETED;
		SPH_SPIN_UNLOCK(&devnet->lock);

		memset(&cmd_args, 0, sizeof(cmd_args));
		cmd_args.devnet_drv_handle = (uint64_t)devnet;
		cmd_args.devnet_rt_handle = (uint64_t)devnet->rt_handle;
		cmd_args.reserve_resource = op->cmd.property_val;
		if (op->cmd.property_val)
			cmd_args.timeout = op->cmd.timeout;

		// kref for RT is taken in find

		ret = inf_cmd_queue_add(&devnet->context->cmdq,
					SPHCS_RUNTIME_CMD_DEVNET_RESOURCES_RESERVATION,
					&cmd_args,
					sizeof(cmd_args),
					NULL, NULL);
		if (unlikely(ret < 0)) {
			sphcs_send_event_report(g_the_sphcs,
						op->cmd.property_val ? SPH_IPC_DEVNET_RESOURCES_RESERVATION_FAILED : SPH_IPC_DEVNET_RESOURCES_RELEASE_FAILED,
						SPH_IPC_NO_MEMORY,
						op->cmd.ctxID,
						op->cmd.netID);
			inf_devnet_put(devnet);
		}
		break;
	}
	default:
		inf_devnet_put(devnet);
		sph_log_err(CREATE_COMMAND_LOG, "unexpected network property (%u)\n", op->cmd.property);
		sphcs_send_event_report(g_the_sphcs,
					SPH_IPC_DEVNET_SET_PROPERTY_FAILED,
					SPH_IPC_NOT_SUPPORTED,
					op->cmd.ctxID,
					op->cmd.netID);
	}

free_op:
	inf_context_put(op->context);
	kfree(op);
}

void IPC_OPCODE_HANDLER(NETWORK_PROPERTY)(struct sphcs *sphcs,
		union h2c_InferenceNetworkProperty *cmd) {
	struct network_property_op_work *work;
	struct inf_context *context;
	enum event_val val;

	context = find_and_get_context(sphcs->inf_data, cmd->ctxID);
	if (unlikely(context == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(work->cmd.value, cmd->value, sizeof(work->cmd.value));
	work->context = context;
	INIT_WORK(&work->work, network_property_op_work_handler);
	queue_work(context->wq, &work->work);

	return;

send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_DEVNET_SET_PROPERTY_FAILED,
				val, cmd->ctxID, cmd->netID);
}

void IPC_OPCODE_HANDLER(CHAN_NETWORK_PROPERTY)(struct sphcs *sphcs,
		union h2c_ChanInferenceNetworkSetProperty *cmd) {
	struct network_property_op_work *work;
	struct inf_context *context;
	enum event_val val;

	context = find_and_get_context(sphcs->inf_data, cmd->chanID);
	if (unlikely(context == NULL || context->chan == NULL)) {
		val = SPH_IPC_NO_SUCH_CONTEXT;
		goto send_error;
	}

	work = kzalloc(sizeof(*work), GFP_NOWAIT);
	if (unlikely(work == NULL)) {
		inf_context_put(context);
		val = SPH_IPC_NO_MEMORY;
		goto send_error;
	}

	memcpy(work->cmd.value, cmd->value, sizeof(work->cmd.value));
	work->cmd.opcode = cmd->opcode;
	work->cmd.ctxID = cmd->chanID;
	work->cmd.netID = cmd->netID;
	work->cmd.timeout = cmd->timeout;
	work->context = context;

	INIT_WORK(&work->work, network_property_op_work_handler);
	queue_work(context->chan->wq, &work->work);

	return;

send_error:
	sphcs_send_event_report(sphcs, SPH_IPC_DEVNET_SET_PROPERTY_FAILED,
				val, cmd->chanID, cmd->netID);
}

static const struct file_operations sphcs_inf_fops = {
	.owner = THIS_MODULE,
	.open = sphcs_inf_open,
	.release = sphcs_inf_release,
	.unlocked_ioctl = sphcs_inf_ioctl,
	.compat_ioctl = sphcs_inf_ioctl,
	.poll = sphcs_inf_poll,
	.read = sphcs_inf_read
};

static inline int is_inf_file(struct file *f)
{
	return f->f_op == &sphcs_inf_fops;
}

int sphcs_alloc_resource(struct sphcs                 *sphcs,
			 uint64_t                      size,
			 uint32_t                      page_size,
			 sphcs_alloc_resource_callback cb,
			 void                          *ctx)
{
	struct inf_alloc_resource cmd_args;
	struct alloc_req *alloc_req;
	int rc;

	if (unlikely(sphcs == NULL))
		return -EINVAL;

	if (unlikely(sphcs->inf_data == NULL))
		return -ENODEV;

	mutex_lock(&sphcs->inf_data->io_lock);
	if (unlikely(sphcs->inf_data->daemon == NULL)) {
		mutex_unlock(&sphcs->inf_data->io_lock);
		return -ENODEV;
	}

	alloc_req = kzalloc(sizeof(*alloc_req), GFP_KERNEL);
	if (unlikely(alloc_req == NULL)) {
		mutex_unlock(&sphcs->inf_data->io_lock);
		return -ENOMEM;
	}
	alloc_req->cb = cb;
	alloc_req->context = ctx;

	SPH_SPIN_LOCK(&sphcs->inf_data->daemon->lock);
	list_add_tail(&alloc_req->node, &sphcs->inf_data->daemon->alloc_req_list);
	SPH_SPIN_UNLOCK(&sphcs->inf_data->daemon->lock);

	cmd_args.drv_handle = (uint64_t)alloc_req;
	cmd_args.size = size;
	cmd_args.page_size = page_size;

	rc = inf_cmd_queue_add(&sphcs->inf_data->daemon->cmdq,
			       SPHCS_DAEMON_CMD_ALLOC_RESOURCE,
			       &cmd_args,
			       sizeof(cmd_args),
			       NULL, NULL);

	if (unlikely(rc < 0)) {
		SPH_SPIN_LOCK(&sphcs->inf_data->daemon->lock);
		list_del(&alloc_req->node);
		SPH_SPIN_UNLOCK(&sphcs->inf_data->daemon->lock);
		mutex_unlock(&sphcs->inf_data->io_lock);
		kfree(alloc_req);
	}

	mutex_unlock(&sphcs->inf_data->io_lock);

	return rc;
}

int sphcs_free_resource(struct sphcs  *sphcs,
			int            dmabuf_fd)
{
	struct inf_free_resource cmd_args;
	int rc;

	if (unlikely(sphcs == NULL))
		return -EINVAL;

	if (unlikely(sphcs->inf_data == NULL))
		return -ENODEV;

	mutex_lock(&sphcs->inf_data->io_lock);
	if (unlikely(sphcs->inf_data->daemon == NULL)) {
		mutex_unlock(&sphcs->inf_data->io_lock);
		return -ENODEV;
	}

	cmd_args.buf_fd = dmabuf_fd;

	rc = inf_cmd_queue_add(&sphcs->inf_data->daemon->cmdq,
			       SPHCS_DAEMON_CMD_FREE_RESOURCE,
			       &cmd_args,
			       sizeof(cmd_args),
			       NULL, NULL);

	mutex_unlock(&sphcs->inf_data->io_lock);

	return rc;
}

static void sphcs_inf_new_data_arrived(struct sphcs_p2p_buf *buf)
{
	struct inf_devres *devres;

	sph_log_debug(START_UP_LOG, "New data arrived (buf id %u)\n", buf->buf_id);

	devres = container_of(buf, struct inf_devres, p2p_buf);
	buf->ready = true;

	/* advance sched tick and try execute next requests */
	atomic_add(2, &devres->context->sched_tick);
	inf_devres_try_execute(devres);
}

static void sphcs_inf_data_consumed(struct sphcs_p2p_buf *buf)
{
	struct inf_devres *devres;

	sph_log_debug(START_UP_LOG, "Data consumed (buf id %u)\n", buf->buf_id);

	devres = container_of(buf, struct inf_devres, p2p_buf);
	buf->ready = true;

	/* advance sched tick and try execute next requests */
	atomic_add(2, &devres->context->sched_tick);
	inf_devres_try_execute(devres);
}

static struct sphcs_p2p_cbs s_p2p_cbs = {
		.new_data_arrived = sphcs_inf_new_data_arrived,
		.data_consumed = sphcs_inf_data_consumed,
};

int inference_init(struct sphcs *sphcs)
{
	struct inf_data *inf_data = NULL;
	int ret;

	ret = alloc_chrdev_region(&s_devnum, 0, 1, SPHCS_INF_DEV_NAME);
	if (unlikely(ret < 0)) {
		sph_log_err(START_UP_LOG, "failed to allocate devnum %d\n", ret);
		return ret;
	}

	cdev_init(&s_cdev, &sphcs_inf_fops);
	s_cdev.owner = THIS_MODULE;

	ret = cdev_add(&s_cdev, s_devnum, 1);
	if (unlikely(ret < 0)) {
		sph_log_err(START_UP_LOG, "failed to add cdev %d\n", ret);
		goto unreg;
	}

	s_class = class_create(THIS_MODULE, SPHCS_INF_DEV_NAME);
	if (unlikely(IS_ERR(s_class))) {
		ret = PTR_ERR(s_class);
		sph_log_err(START_UP_LOG, "failed to register class %d\n", ret);
		goto free_cdev;
	}

	s_dev = device_create(s_class, NULL, s_devnum, NULL, SPHCS_INF_DEV_NAME);
	if (unlikely(IS_ERR(s_dev))) {
		ret = PTR_ERR(s_dev);
		goto free_class;
	}

	inf_data = kzalloc(sizeof(*inf_data), GFP_KERNEL);
	if (unlikely(inf_data == NULL)) {
		ret = -ENOMEM;
		goto free_dev;
	}

	spin_lock_init(&inf_data->lock_bh);
	mutex_init(&inf_data->io_lock);
	hash_init(inf_data->context_hash);

	ret = sphcs_ctx_uids_init();
	if (ret) {
		sph_log_err(START_UP_LOG, "Failed to initialize ctx_uids sysfs counters");
		goto free_mutex;
	}

	inf_data->inf_wq = create_singlethread_workqueue("sphcs_inf_wq");
	if (!inf_data->inf_wq) {
		sph_log_err(START_UP_LOG, "Failed to initialize ctx create/destroy workqueue");
		goto free_ctx_uids;
	}

	sphcs->inf_data = inf_data;

	ret = sphcs_p2p_init(sphcs, &s_p2p_cbs);
	if (ret) {
		sph_log_err(START_UP_LOG, "Failed to initialize p2p");
		goto destroy_wq;
	}

	return 0;

destroy_wq:
	destroy_workqueue(sphcs->inf_data->inf_wq);
free_ctx_uids:
	sphcs_ctx_uids_fini();
free_mutex:
	mutex_destroy(&sphcs->inf_data->io_lock);
free_dev:
	device_destroy(s_class, s_devnum);
free_class:
	class_destroy(s_class);
free_cdev:
	cdev_del(&s_cdev);
unreg:
	unregister_chrdev_region(s_devnum, 1);
	kfree(inf_data);

	return ret;
}

int inference_fini(struct sphcs *sphcs)
{
	sphcs_p2p_fini(sphcs);
	sphcs_ctx_uids_fini();
	destroy_workqueue(sphcs->inf_data->inf_wq);
	device_destroy(s_class, s_devnum);
	class_destroy(s_class);
	cdev_del(&s_cdev);
	unregister_chrdev_region(s_devnum, 1);

	// TODO: remove resources
	mutex_destroy(&sphcs->inf_data->io_lock);
	kfree(sphcs->inf_data);
	return 0;
}

static void release_pending_create_context_reuquests(void *cmd_args)
{
	struct inf_create_context *ctx_cmd_args = cmd_args;
	struct inf_context *context = find_and_get_context(g_the_sphcs->inf_data, ctx_cmd_args->contextID);

	SPH_ASSERT(context != NULL);
	SPH_ASSERT(!context->daemon_ref_released);
	inf_context_put(context);

	sph_log_debug(START_UP_LOG, "Release pending create context requests ID %d\n", ctx_cmd_args->contextID);
	sphcs_send_event_report(g_the_sphcs,
				SPH_IPC_CREATE_CONTEXT_FAILED,
				SPH_IPC_NO_DAEMON,
				ctx_cmd_args->contextID,
				-1);

	destroy_context_on_create_failed(g_the_sphcs, context);
	context->daemon_ref_released = true;
	inf_context_put(context);
}

static int sched_status_show(struct seq_file *m, void *v)
{
	struct inf_data *inf_data;
	struct inf_context *context;
	struct inf_req_sequence *seq;
	struct inf_exec_req *req;
	unsigned long flags;
	u64 curr_time;
	int i;
	int num_contexts = 0;
	struct sysinfo sinfo;

	if (!g_the_sphcs)
		return -1;

	curr_time = sph_time_us();
	inf_data = g_the_sphcs->inf_data;
	SPH_SPIN_LOCK_BH(&inf_data->lock_bh);
	hash_for_each(inf_data->context_hash, i, context, hash_node) {
		num_contexts++;
		SPH_SPIN_LOCK_IRQSAVE(&context->sync_lock_irq, flags);
		if (!context->attached || context->destroyed || context->runtime_detach_sent)
			seq_printf(m, "Context %d attach state: attached=%d destroyed=%d runtime_detach_sent=%d\n",
				   context->protocolID,
				   context->attached,
				   context->destroyed,
				   context->runtime_detach_sent);
		if (list_empty(&context->active_seq_list)) {
			seq_printf(m, "Context %d: No scheduled commands, state=%d\n", context->protocolID, context->state);
		} else {
			seq_printf(m, "Context %d: state=%d\n", context->protocolID, context->state);
			list_for_each_entry(seq, &context->active_seq_list, node) {
				req = container_of(seq,
						   struct inf_exec_req,
						   seq);
				if (req->cmd_type == CMDLIST_CMD_INFREQ) {
					seq_printf(m, "\tinfer command %d network %d\n",
						  req->infreq->protocolID,
						  req->infreq->devnet->protocolID);
					if (req->cmd != NULL)
						seq_printf(m, "\t\tcommand list %d\n", req->cmd->protocolID);
					seq_printf(m, "\t\tin_progress: %d\n", req->in_progress);
					seq_printf(m, "\t\tpriority: %d\n", req->priority);
					seq_printf(m, "\t\ttime: %lld\n", curr_time - req->time);
				} else if (req->cmd_type == CMDLIST_CMD_COPY) {
					seq_printf(m, "\tcopy command %d\n", req->copy->protocolID);
					if (req->cmd != NULL)
						seq_printf(m, "\t\tcommand list %d\n", req->cmd->protocolID);
					seq_printf(m, "\t\tin_progress: %d\n", req->in_progress);
					seq_printf(m, "\t\tpriority: %d\n", req->priority);
					seq_printf(m, "\t\ttime: %lld\n", curr_time - req->time);
				} else if (req->cmd_type == CMDLIST_CMD_COPYLIST) {
					seq_printf(m, "\tcopy list command idx=%d, n_copies=%d\n",
						   req->cpylst->idx_in_cmd,
						   req->cpylst->n_copies);
					if (req->cmd != NULL)
						seq_printf(m, "\t\tcommand list %d\n", req->cmd->protocolID);
					seq_printf(m, "\t\tin_progress: %d\n", req->in_progress);
					seq_printf(m, "\t\tpriority: %d\n", req->priority);
					seq_printf(m, "\t\ttime: %lld\n", curr_time - req->time);
				} else {
					seq_printf(m, "\tUNKNWON COMMAND TYPE %d !!!\n", req->cmd_type);
				}
			}
		}
		SPH_SPIN_UNLOCK_IRQRESTORE(&context->sync_lock_irq, flags);
	}
	SPH_SPIN_UNLOCK_BH(&inf_data->lock_bh);

	if (num_contexts == 0)
		seq_puts(m, "No active contexts\n");

	si_meminfo(&sinfo);
	seq_puts(m, "\nMemory Info:\n");
	seq_printf(m, "TotalRam: %lu KB\n", sinfo.totalram * (sinfo.mem_unit >> 10));
	seq_printf(m, "FreeRam: %lu KB\n", sinfo.freeram * (sinfo.mem_unit >> 10));

	return 0;
}

static int sched_status_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_status_show, inode->i_private);
}

static const struct file_operations sched_status_fops = {
	.open		= sched_status_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

void sphcs_inf_init_debugfs(struct dentry *parent)
{
	debugfs_create_file("sched_status",
			    0444,
			    parent,
			    NULL,
			    &sched_status_fops);
}
