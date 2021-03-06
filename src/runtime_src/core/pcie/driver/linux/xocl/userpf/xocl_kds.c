// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo User Function Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: min.ma@xilinx.com
 */

#include <linux/workqueue.h>
#include "common.h"
#include "kds_core.h"

int kds_mode = 0;
module_param(kds_mode, int, (S_IRUGO|S_IWUSR));
MODULE_PARM_DESC(kds_mode,
		 "enable new KDS (0 = disable (default), 1 = enable)");

/* kds_echo also impact mb_scheduler.c, keep this as global.
 * Let's move it to struct kds_sched in the future.
 */
int kds_echo = 0;

static inline void
xocl_ctx_to_info(struct drm_xocl_ctx *args, struct kds_ctx_info *info)
{
	if (args->cu_index == XOCL_CTX_VIRT_CU_INDEX)
		info->cu_idx = CU_CTX_VIRT_CU;
	else
		info->cu_idx = args->cu_index;

	if (args->flags == XOCL_CTX_EXCLUSIVE)
		info->flags = CU_CTX_EXCLUSIVE;
	else
		info->flags = CU_CTX_SHARED;
}

static int xocl_add_context(struct xocl_dev *xdev, struct kds_client *client,
			    struct drm_xocl_ctx *args)
{
	struct kds_ctx_info	 info;
	xuid_t *uuid;
	int ret;

	mutex_lock(&client->lock);
	/* If this client has no opened context, lock bitstream */
	if (!client->num_ctx) {
		ret = xocl_icap_lock_bitstream(xdev, &args->xclbin_id);
		if (ret)
			goto out;
		uuid = vzalloc(sizeof(*uuid));
		if (!uuid) {
			ret = -ENOMEM;
			goto out;
		}
		uuid_copy(uuid, &args->xclbin_id);
		client->xclbin_id = uuid;
	}

	/* Bitstream is locked. No one could load a new one
	 * until this client close all of the contexts.
	 */
	xocl_ctx_to_info(args, &info);
	ret = kds_add_context(&XDEV(xdev)->kds, client, &info);

out:
	if (!client->num_ctx) {
		vfree(client->xclbin_id);
		client->xclbin_id = NULL;
		(void) xocl_icap_unlock_bitstream(xdev, &args->xclbin_id);
	}
	mutex_unlock(&client->lock);
	return ret;
}

static int xocl_del_context(struct xocl_dev *xdev, struct kds_client *client,
			    struct drm_xocl_ctx *args)
{
	struct kds_ctx_info	 info;
	xuid_t *uuid;
	int ret = 0;

	mutex_lock(&client->lock);

	uuid = client->xclbin_id;
	/* xclCloseContext() would send xclbin_id and cu_idx.
	 * Be more cautious while delete. Do sanity check */
	if (!uuid) {
		userpf_err(xdev, "No context was opened");
		ret = -EINVAL;
		goto out;
	}

	/* If xclbin id looks good, unlock bitstream should not fail. */
	if (!uuid_equal(uuid, &args->xclbin_id)) {
		userpf_err(xdev, "Try to delete CTX on wrong xclbin");
		ret = -EBUSY;
		goto out;
	}

	xocl_ctx_to_info(args, &info);
	ret = kds_del_context(&XDEV(xdev)->kds, client, &info);
	if (ret)
		goto out;

	/* unlock bitstream if there is no opening context */
	if (!client->num_ctx) {
		vfree(client->xclbin_id);
		client->xclbin_id = NULL;
		(void) xocl_icap_unlock_bitstream(xdev, &args->xclbin_id);
	}

out:
	mutex_unlock(&client->lock);
	return ret;
}

static int xocl_context_ioctl(struct xocl_dev *xdev, void *data,
			      struct drm_file *filp)
{
	struct drm_xocl_ctx *args = data;
	struct kds_client *client = filp->driver_priv;
	int ret = 0;

	switch(args->op) {
	case XOCL_CTX_OP_ALLOC_CTX:
		ret = xocl_add_context(xdev, client, args);
		break;
	case XOCL_CTX_OP_FREE_CTX:
		ret = xocl_del_context(xdev, client, args);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void notify_execbuf(struct kds_command *xcmd, int status)
{
	struct kds_client *client = xcmd->client;
	struct ert_packet *ecmd = (struct ert_packet *)xcmd->execbuf;

	if (status == KDS_COMPLETED)
		ecmd->state = ERT_CMD_STATE_COMPLETED;
	else if (status == KDS_ERROR)
		ecmd->state = ERT_CMD_STATE_ERROR;
	else if (status == KDS_TIMEOUT)
		ecmd->state = ERT_CMD_STATE_TIMEOUT;
	else if (status == KDS_ABORT)
		ecmd->state = ERT_CMD_STATE_ABORT;

	XOCL_DRM_GEM_OBJECT_PUT_UNLOCKED(xcmd->gem_obj);

	if (xcmd->inkern_cb) {
		schedule_work(&xcmd->inkern_cb->work);
	} else {
		atomic_inc(&client->event);
		wake_up_interruptible(&client->waitq);
	}
}

static void xocl_execbuf_completion(struct work_struct *work)
{
	struct in_kernel_cb *inkern_cb = container_of(work,
						struct in_kernel_cb, work);
	int error = (inkern_cb->cmd_state == ERT_CMD_STATE_COMPLETED) ?
			0 : -EFAULT;

	if (inkern_cb->func)
		inkern_cb->func((unsigned long)inkern_cb->data, error);
}

static int xocl_command_ioctl(struct xocl_dev *xdev, void *data,
			      struct drm_file *filp, bool in_kernel)
{
	struct drm_device *ddev = filp->minor->dev;
	struct kds_client *client = filp->driver_priv;
	struct drm_xocl_execbuf *args = data;
	struct drm_gem_object *obj;
	struct drm_xocl_bo *xobj;
	struct ert_packet *ecmd;
	struct kds_command *xcmd;
	int ret = 0;

	if (!client->xclbin_id) {
		userpf_err(xdev, "The client has no opening context\n");
		return -EINVAL;
	}

	if (XDEV(xdev)->kds.bad_state) {
		userpf_err(xdev, "KDS is in bad state\n");
		return -EDEADLK;
	}

	obj = xocl_gem_object_lookup(ddev, filp, args->exec_bo_handle);
	if (!obj) {
		userpf_err(xdev, "Failed to look up GEM BO %d\n",
		args->exec_bo_handle);
		return -ENOENT;
	}

	xobj = to_xocl_bo(obj);
	if (!xocl_bo_execbuf(xobj)) {
		ret = -EINVAL;
		goto out;
	}

	ecmd = (struct ert_packet *)xobj->vmapping;

	ecmd->state = ERT_CMD_STATE_NEW;
	/* only the user command knows the real size of the payload.
	 * count is more than enough!
	 */
	xcmd = kds_alloc_command(client, ecmd->count * sizeof(u32));
	if (!xcmd) {
		userpf_err(xdev, "Failed to alloc xcmd\n");
		ret = -ENOMEM;
		goto out;
	}
	xcmd->cb.free = kds_free_command;

	/* TODO: one ecmd to one xcmd now. Maybe we will need
	 * one ecmd to multiple xcmds
	 */
	if (ecmd->opcode == ERT_CONFIGURE) {
		cfg_ecmd2xcmd(to_cfg_pkg(ecmd), xcmd);
	} else if (ecmd->opcode == ERT_START_CU)
		start_krnl_ecmd2xcmd(to_start_krnl_pkg(ecmd), xcmd);

	if (XDEV(xdev)->kds.ert_disable)
		xcmd->type = KDS_CU;
	else
		xcmd->type = KDS_ERT;

	xcmd->cb.notify_host = notify_execbuf;
	xcmd->gem_obj = obj;

	if (in_kernel) {
		struct drm_xocl_execbuf_cb *args_cb =
					(struct drm_xocl_execbuf_cb *)data;

		if (args_cb->cb_func) {
			xcmd->inkern_cb = kzalloc(sizeof(struct in_kernel_cb),
								GFP_KERNEL);
			if (!xcmd->inkern_cb) {
				xcmd->cb.free(xcmd);
				ret = -ENOMEM;
				goto out;
			}
			xcmd->inkern_cb->func = (void (*)(unsigned long, int))
						args_cb->cb_func;
			xcmd->inkern_cb->data = (void *)args_cb->cb_data;
			INIT_WORK(&xcmd->inkern_cb->work,
						xocl_execbuf_completion);
		}
	}

	/* Now, we could forget execbuf */
	ret = kds_add_command(&XDEV(xdev)->kds, xcmd);

out:
	return ret;
}

int xocl_create_client(struct xocl_dev *xdev, void **priv)
{
	struct	kds_client	*client;
	struct  kds_sched	*kds;
	int	ret = 0;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	kds = &XDEV(xdev)->kds;
	client->dev = XDEV2DEV(xdev);
	ret = kds_init_client(kds, client);
	if (ret) {
		kfree(client);
		goto out;
	}

	*priv = client;

out:
	userpf_info(xdev, "created KDS client for pid(%d), ret: %d\n",
		    pid_nr(task_tgid(current)), ret);
	return ret;
}

void xocl_destroy_client(struct xocl_dev *xdev, void **priv)
{
	struct kds_client *client = *priv;
	struct kds_sched  *kds;
	int pid = pid_nr(client->pid);

	kds = &XDEV(xdev)->kds;
	kds_fini_client(kds, client);
	if (client->xclbin_id) {
		(void) xocl_icap_unlock_bitstream(xdev, client->xclbin_id);
		vfree(client->xclbin_id);
	}
	kfree(client);
	userpf_info(xdev, "client exits pid(%d)\n", pid);
}

int xocl_poll_client(struct file *filp, poll_table *wait, void *priv)
{
	struct kds_client *client = (struct kds_client *)priv;
	int event;

	poll_wait(filp, &client->waitq, wait);

	event = atomic_dec_if_positive(&client->event);
	if (event == -1)
		return 0;

	/* If only return POLLIN, I could get 100K IOPS more.
	 * With above wait, the IOPS is more unstable (+/-100K).
	 */
	return POLLIN;
}

int xocl_client_ioctl(struct xocl_dev *xdev, int op, void *data,
		      struct drm_file *filp)
{
	int ret = 0;

	switch (op) {
	case DRM_XOCL_CTX:
		ret = xocl_context_ioctl(xdev, data, filp);
		break;
	case DRM_XOCL_EXECBUF:
		ret = xocl_command_ioctl(xdev, data, filp, false);
		break;
	case DRM_XOCL_EXECBUF_CB:
		ret = xocl_command_ioctl(xdev, data, filp, true);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

int xocl_init_sched(struct xocl_dev *xdev)
{
	return kds_init_sched(&XDEV(xdev)->kds);
}

void xocl_fini_sched(struct xocl_dev *xdev)
{
	kds_fini_sched(&XDEV(xdev)->kds);
}

int xocl_kds_stop(struct xocl_dev *xdev)
{
	/* plact holder */
	return 0;
}

int xocl_kds_reset(struct xocl_dev *xdev, const xuid_t *xclbin_id)
{
	kds_reset(&XDEV(xdev)->kds);
	return 0;
}

int xocl_kds_reconfig(struct xocl_dev *xdev)
{
	/* plact holder */
	return 0;
}

int xocl_cu_map_addr(struct xocl_dev *xdev, u32 cu_idx,
		     void *drm_filp, u32 *addrp)
{
	/* plact holder */
	return 0;
}

u32 xocl_kds_live_clients(struct xocl_dev *xdev, pid_t **plist)
{
	return kds_live_clients(&XDEV(xdev)->kds, plist);
}

void xocl_kds_update(struct xocl_dev *xdev)
{
	if (xocl_ert_30_cu_intr_cfg(xdev) == -ENODEV) {
		userpf_info(xdev, "Not support CU to host interrupt");
		XDEV(xdev)->kds.cu_intr_cap = 0;
	} else {
		userpf_info(xdev, "Shell supports CU to host interrupt");
		XDEV(xdev)->kds.cu_intr_cap = 1;
	}

	XDEV(xdev)->kds.cu_intr = 0;
	kds_cfg_update(&XDEV(xdev)->kds);
}
