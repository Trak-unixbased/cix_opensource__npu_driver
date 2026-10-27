// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023-2025 Arm Technology (China) Co. Ltd. */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/compat.h>
#include <armchina_aipu.h>
#include "armchina_aipu_soc.h"
#include "aipu_mm.h"
#include "aipu_dma_buf.h"
#include "aipu_job_manager.h"
#include "aipu_priv.h"
#include "zhouyi.h"
#include "config.h"
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>

#ifdef CONFIG_SKY1
#include "cix_sky1_soc.h"
#endif

/*
 * Single AIPU instance shared by all partitions/cores. It is allocated with
 * devm_kzalloc() on the first probed platform device, so it is only valid
 * while that device is bound: it is published here only once fully
 * initialised, and cleared on failure and on remove so that a deferred
 * probe (-EPROBE_DEFER) or an unbind/bind cycle never reuses freed memory.
 */
static struct aipu_priv *aipu;

/* /dev/aipu is usable only once the job manager AND partitions exist. */
static bool aipu_is_ready(struct aipu_priv *priv)
{
	return priv && !aipu_priv_check_status(priv) && priv->partitions;
}

static int aipu_open(struct inode *inode, struct file *filp)
{
	struct aipu_priv *priv = aipu;
	int ret;

	/*
	 * misc_register() runs inside init_aipu_priv(), i.e. before the job
	 * manager and partitions are ready: refuse early openers.
	 */
	if (!aipu_is_ready(priv))
		return -ENODEV;

#ifdef CONFIG_SKY1
	/* resume_and_get drops the usage count itself on failure */
	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret < 0) {
		dev_err(priv->dev, "open: runtime resume failed: %d\n", ret);
		return ret;
	}
#endif

	filp->private_data = priv;
	return 0;
}

static int aipu_release(struct inode *inode, struct file *filp)
{
	int ret;
	struct aipu_priv *priv = filp->private_data;

	if (!priv)
		return 0;

	/*
	 * The VFS ignores the return value of ->release(): never bail out
	 * early, otherwise the buffers and the runtime PM reference of this
	 * file are leaked forever (NPU never suspends again).
	 */
	ret = aipu_job_manager_cancel_jobs(&priv->job_manager, filp);
	if (ret)
		dev_warn(priv->dev, "release: cancel jobs failed: %d\n", ret);

	aipu_mm_free_buffers(&priv->mm, filp);

#ifdef CONFIG_SKY1
	pm_runtime_put(priv->dev);
#endif

	return 0;
}

static long aipu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct aipu_priv *aipu = filp->private_data;
	struct aipu_job_manager *manager = &aipu->job_manager;
	void __user *uarg = (void __user *)arg;

	u32 partition_cnt = 0;
	struct aipu_partition_cap *partition_cap = NULL;
	struct aipu_cap cap;
	struct aipu_buf_request buf_req;
	struct aipu_job_desc user_job;
	struct aipu_buf_desc desc;
	struct aipu_io_req io_req;
	struct aipu_job_status_query status;
	struct aipu_hw_status hw;
	struct aipu_config_clusters config_clusters;
	struct aipu_dma_buf_request dmabuf_req;
	struct aipu_dma_buf dmabuf_info;
	struct aipu_id_desc id_desc;
	struct aipu_cluster_status cluster_status;
	struct aipu_running_job_query running_job_query;
	int fd = 0;
	int id = 0;

	u64 job_id;
	struct aipu_rebind_buf_desc rebind_buf;
	struct aipu_bind_buf_desc bind_buf;

	/*
	 * Convention: a failed copy_{from,to}_user() returns -EFAULT, never
	 * the raw "bytes not copied" count (positive value seen as success by
	 * the UMD) nor a silent 0.
	 */
	switch (cmd) {
	case AIPU_IOCTL_QUERY_CAP:
		ret = aipu_priv_query_capability(aipu, &cap);
		if (!ret && copy_to_user(uarg, &cap, sizeof(cap)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_QUERY_PARTITION_CAP:
		partition_cnt = aipu_priv_get_partition_cnt(aipu);
		if (!partition_cnt) {
			ret = -ENODEV;
			break;
		}
		partition_cap = kcalloc(partition_cnt, sizeof(*partition_cap), GFP_KERNEL);
		if (!partition_cap) {
			ret = -ENOMEM;
			break;
		}
		ret = aipu_priv_query_partition_capability(aipu, partition_cap);
		if (!ret && copy_to_user(uarg, partition_cap,
					 partition_cnt * sizeof(*partition_cap)))
			ret = -EFAULT;
		kfree(partition_cap);
		break;
	case AIPU_IOCTL_REQ_BUF:
		if (copy_from_user(&buf_req, uarg, sizeof(buf_req))) {
			ret = -EFAULT;
			break;
		}
		if (!aipu->mm.has_iommu)
			buf_req.alloc_mode = AIPU_DMA_BUF_MALLOC_DEFAULT;

		if (buf_req.alloc_mode == AIPU_DMA_BUF_MALLOC_IOVA ||
		    buf_req.alloc_mode == AIPU_DMA_BUF_MALLOC_IOVA_PHY) {
			ret = aipu_job_manager_alloc_exec_id(manager, &buf_req.exec_id);
			if (ret)
				break;
		} else {
			buf_req.exec_id = 0;
		}
		ret = aipu_alloc_dma_iova_phy(&aipu->mm, &buf_req, filp);
		if (!ret && copy_to_user(uarg, &buf_req, sizeof(buf_req)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_FREE_BUF:
		if (!copy_from_user(&desc, uarg, sizeof(desc)))
			ret = aipu_free_dma_iova_phy(&aipu->mm, &desc, filp);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_DISABLE_SRAM:
		ret = aipu_mm_disable_sram_allocation(&aipu->mm, filp);
		break;
	case AIPU_IOCTL_ENABLE_SRAM:
		ret = aipu_mm_enable_sram_allocation(&aipu->mm, filp);
		break;
	case AIPU_IOCTL_SCHEDULE_JOB:
		if (!copy_from_user(&user_job, uarg, sizeof(user_job)))
			ret = aipu_job_manager_scheduler(manager, &user_job, filp);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_QUERY_STATUS:
		/* original code returned 0 (success) when copy_from_user failed */
		if (copy_from_user(&status, uarg, sizeof(status))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_job_manager_get_job_status(manager, &status, filp);
		if (!ret && copy_to_user(uarg, &status, sizeof(status)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_KILL_TIMEOUT_JOB:
		if (!copy_from_user(&job_id, uarg, sizeof(job_id)))
			ret = aipu_job_manager_invalidate_timeout_job(manager, job_id);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_REQ_IO:
		if (copy_from_user(&io_req, uarg, sizeof(io_req))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_priv_io_rw(aipu, &io_req);
		if (!ret && copy_to_user(uarg, &io_req, sizeof(io_req)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_GET_HW_STATUS:
		ret = aipu_job_manager_get_hw_status(manager, &hw);
		if (!ret && copy_to_user(uarg, &hw, sizeof(hw)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_ABORT_CMD_POOL:
		ret = aipu_job_manager_abort_cmd_pool(manager);
		break;
	case AIPU_IOCTL_DISABLE_TICK_COUNTER:
		ret = aipu_job_manager_disable_tick_counter(manager);
		break;
	case AIPU_IOCTL_ENABLE_TICK_COUNTER:
		ret = aipu_job_manager_enable_tick_counter(manager);
		break;
	case AIPU_IOCTL_CONFIG_CLUSTERS:
		if (!copy_from_user(&config_clusters, uarg, sizeof(config_clusters)))
			ret = aipu_job_manager_config_clusters(manager, &config_clusters);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_ALLOC_DMA_BUF:
		if (copy_from_user(&dmabuf_req, uarg, sizeof(dmabuf_req))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_alloc_dma_buf(&aipu->mm, &dmabuf_req);
		if (!ret && copy_to_user(uarg, &dmabuf_req, sizeof(dmabuf_req)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_FREE_DMA_BUF:
		if (!copy_from_user(&fd, uarg, sizeof(fd)))
			ret = aipu_free_dma_buf(&aipu->mm, fd);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_GET_DMA_BUF_INFO:
		if (copy_from_user(&dmabuf_info, uarg, sizeof(dmabuf_info))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_get_dma_buf_info(&dmabuf_info);
		if (!ret && copy_to_user(uarg, &dmabuf_info, sizeof(dmabuf_info)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_ATTACH_DMA_BUF:
		if (copy_from_user(&dmabuf_info, uarg, sizeof(dmabuf_info))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_attach_dma_buf(&aipu->mm, &dmabuf_info);
		if (!ret && copy_to_user(uarg, &dmabuf_info, sizeof(dmabuf_info)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_DETACH_DMA_BUF:
		if (!copy_from_user(&fd, uarg, sizeof(fd)))
			ret = aipu_detach_dma_buf(&aipu->mm, fd);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_GET_DRIVER_VERSION:
		/* copy_to_user() returns the number of bytes NOT copied */
		if (copy_to_user(uarg, KMD_VERSION, sizeof(KMD_VERSION)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_ALLOC_GRID_ID:
		/*
		 * Original code reused 'ret' both as the ID and as the copy
		 * status, so a successful call returned 0 and a partial copy
		 * returned a positive byte count.
		 */
		id = aipu_job_manager_alloc_grid_id(manager);
		if (id < 0)
			ret = id;
		else if (copy_to_user(uarg, &id, sizeof(id)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_ALLOC_GROUP_ID:
		if (copy_from_user(&id_desc, uarg, sizeof(id_desc))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_job_manager_alloc_group_id(manager, &id_desc);
		if (!ret && copy_to_user(uarg, &id_desc, sizeof(id_desc)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_FREE_GROUP_ID:
		if (!copy_from_user(&id_desc, uarg, sizeof(id_desc)))
			ret = aipu_job_manager_free_group_id(manager, &id_desc);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_GET_CLUSTER_STATUS:
		if (copy_from_user(&cluster_status, uarg, sizeof(cluster_status))) {
			ret = -EFAULT;
			break;
		}
		if (!aipu->ops || !aipu->ops->get_partition_status) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = aipu->ops->get_partition_status(aipu, &cluster_status);
		if (!ret && copy_to_user(uarg, &cluster_status, sizeof(cluster_status)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_GET_RUNNING_JOB_THREAD_ID:
		if (copy_from_user(&running_job_query, uarg, sizeof(running_job_query))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_job_manager_get_running_job_thread(manager, &running_job_query);
		if (!ret && copy_to_user(uarg, &running_job_query, sizeof(running_job_query)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_ALLOC_SFLAG_ID:
		if (copy_from_user(&id_desc, uarg, sizeof(id_desc))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_job_manager_alloc_sflag_id(manager, &id_desc);
		if (!ret && copy_to_user(uarg, &id_desc, sizeof(id_desc)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_FREE_SFLAG_ID:
		if (!copy_from_user(&id_desc, uarg, sizeof(id_desc)))
			ret = aipu_job_manager_free_sflag_id(manager, &id_desc);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_REBIND_DMA_BUF:
		if (copy_from_user(&rebind_buf, uarg, sizeof(rebind_buf))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_rebind_dma_iova_phy(&aipu->mm, &rebind_buf, filp);
		if (!ret && copy_to_user(uarg, &rebind_buf, sizeof(rebind_buf)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_BIND_DMA_BUF:
		if (copy_from_user(&bind_buf, uarg, sizeof(bind_buf))) {
			ret = -EFAULT;
			break;
		}
		ret = aipu_bind_dma_iova_phy(&aipu->mm, &bind_buf, filp);
		if (!ret && copy_to_user(uarg, &bind_buf, sizeof(bind_buf)))
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_BUF_CACHE_INVALID:
		if (!copy_from_user(&desc, uarg, sizeof(desc)))
			ret = aipu_mm_cache_invalid(&aipu->mm, &desc);
		else
			ret = -EFAULT;
		break;
	case AIPU_IOCTL_BUF_CACHE_FLUSH:
		if (!copy_from_user(&desc, uarg, sizeof(desc)))
			ret = aipu_mm_cache_flush(&aipu->mm, &desc);
		else
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long aipu_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	arg = (unsigned long)compat_ptr(arg);

	return aipu_ioctl(filp, cmd, arg);
}
#endif

static int aipu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct aipu_priv *aipu = filp->private_data;

	return aipu_mm_mmap_buf(&aipu->mm, vma, filp);
}

static __poll_t aipu_poll(struct file *filp, struct poll_table_struct *wait)
{
	__poll_t mask = 0;
	struct aipu_priv *aipu = filp->private_data;

	if (aipu_job_manager_has_end_job(&aipu->job_manager, filp, wait, task_pid_nr(current)))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static const struct file_operations aipu_fops = {
	.owner = THIS_MODULE,
	.open = aipu_open,
	.poll = aipu_poll,
	.unlocked_ioctl = aipu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aipu_compat_ioctl,
#endif
	.mmap = aipu_mmap,
	.release = aipu_release,
};

/**
 * @armchina_aipu_probe() - probe operation for platfom driver provided by ArmChina
 * @p_dev: pointer to the AIPU platform device struct.
 * @soc:   pointer to aipu SoC struct contains soc specific data.
 *         this argument can be NULL if SoC vendor has no AIPU related soc data structure.
 * @ops:   pointer to aipu SoC operation struct.
 *         this argument can be NULL if SoC vendor does not provide any soc operation. In
 *         this case the SoC related operations in AIPU driver are unavailable.
 *
 * This function should be called in a SoC vendor provided xx_aipu_probe() function,
 * which is registered to the platfom_driver struct; if no such xx_aipu_probe() is provided,
 * SoC vendor should directly register this function to the platfom_driver struct.
 *
 * Return: 0 on success and error code otherwise.
 */
int armchina_aipu_probe(struct platform_device *p_dev, struct aipu_soc *soc,
			struct aipu_soc_operations *ops)
{
	int ret = 0;
	struct device *dev = &p_dev->dev;
	struct aipu_partition *partition = NULL;
	struct aipu_priv *priv = aipu;
	u32 id = 0;

	/* create & init aipu priv struct shared by all clusters/cores */
	if (!priv) {
		priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;

		dev_info(dev, "AIPU KMD (v%s) probe start...\n", KMD_VERSION);

		platform_set_drvdata(p_dev, priv);

		/*
		 * Publish the pointer before init: misc_register() happens
		 * inside, and aipu_open() relies on is_init/partitions checks
		 * (aipu_is_ready) to reject early openers.
		 */
		aipu = priv;
		ret = init_aipu_priv(priv, p_dev, &aipu_fops, soc, ops);
		if (ret) {
			/* devm memory is released when probe fails: forget it */
			aipu = NULL;
			return ret;
		}
	}

	device_property_read_u32(dev, "core-id", &id);

	if (WARN_ON(!priv->ops || !priv->ops->create_partitions)) {
		ret = -EINVAL;
		goto out_clean;
	}

	partition = priv->ops->create_partitions(priv, id, p_dev);
	if (IS_ERR_OR_NULL(partition)) {
		/* original code returned 0 here: probe "succeeded" on a dead NPU */
		ret = partition ? PTR_ERR(partition) : -ENODEV;
		dev_err(dev, "create partitions failed: %d\n", ret);
		goto out_clean;
	}

	return 0;

out_clean:
	armchina_aipu_remove(p_dev);
	return ret;
}
EXPORT_SYMBOL(armchina_aipu_probe);

/**
 * @armchina_aipu_remove() - remove operation for platfom driver provided by ArmChina
 * @p_dev: pointer to the AIPU platform device struct
 *
 * This function should be called in a SoC vendor provided xx_aipu_remove() function,
 * which is registered to the platfom_driver struct; if no such xx_aipu_remove() is provided,
 * this function should be directly registered to the platfom_driver struct.
 *
 * Return: 0 on success and error code otherwise.
 */
int armchina_aipu_remove(struct platform_device *p_dev)
{
	struct aipu_priv *priv = aipu;
	int ret = 0;

	if (!priv)
		return 0;

	if (priv->is_init)
		ret = deinit_aipu_priv(priv);

	/* priv is devm-managed and about to be freed: drop the global ref */
	aipu = NULL;
	return ret;
}
EXPORT_SYMBOL(armchina_aipu_remove);

/* Common guard for the PM callbacks; returns NULL when nothing to do. */
static struct aipu_job_manager *aipu_pm_manager(const char *who)
{
	struct aipu_job_manager *manager;

	/* original code dereferenced aipu->partitions before any NULL check */
	if (!aipu || !aipu->is_init || !aipu->partitions) {
		pr_debug("aipu %s: npu driver is not ready.\n", who);
		return NULL;
	}

	manager = get_job_manager(aipu->partitions);
	if (!manager)
		pr_debug("aipu %s: npu driver is not ready.\n", who);

	return manager;
}

/**
 * @armchina_aipu_suspend() - suspend operation for platfom driver provided by ArmChina
 * @p_dev: pointer to the AIPU platform device struct.
 * @state: power state device is entering.
 *
 * This function disables the AIPU clock from SoC level with the disable_clk method
 * registered in SoC probing. SoC vendor can directly register this function to the
 * platfom_driver struct or implements its private xx_aipu_suspend() as a replacement.
 */
int armchina_aipu_suspend(struct platform_device *p_dev, pm_message_t state)
{
	int ret;
	struct aipu_job_manager *manager = aipu_pm_manager("suspend");

	if (!manager)
		return 0;

	/* quiesce the NPU first (registers need the clock), then gate it */
	ret = aipu_job_manager_suspend(manager);
	if (ret)
		return ret;

	if (aipu->soc_ops && aipu->soc_ops->disable_clk)
		ret = aipu->soc_ops->disable_clk(aipu->partitions->dev, aipu->soc);

	return ret;
}
EXPORT_SYMBOL(armchina_aipu_suspend);

/**
 * @armchina_aipu_resume() - resume operation for platfom driver provided by ArmChina
 * @p_dev: pointer to the AIPU platform device struct.
 *
 * This function enables the AIPU clock from SoC level with the enable_clk method
 * registered in SoC probing, and enable AIPU interrupts. SoC vendor should directly
 * register this function to the platfom_driver struct, or implements its private
 * xx_aipu_resume() calls this function.
 */
int armchina_aipu_resume(struct platform_device *p_dev)
{
	int ret;
	struct aipu_job_manager *manager = aipu_pm_manager("resume");

	if (!manager)
		return 0;

	/*
	 * Original order was resume (register writes on V3) THEN enable_clk:
	 * touching an unclocked IP on arm64 typically ends in an SError.
	 */
	if (aipu->soc_ops && aipu->soc_ops->enable_clk) {
		ret = aipu->soc_ops->enable_clk(aipu->partitions->dev, aipu->soc);
		if (ret)
			return ret;
	}

	ret = aipu_job_manager_resume(manager);
	if (ret && aipu->soc_ops && aipu->soc_ops->disable_clk)
		aipu->soc_ops->disable_clk(aipu->partitions->dev, aipu->soc);

	return ret;
}
EXPORT_SYMBOL(armchina_aipu_resume);

/**
 * @armchina_aipu_alloc_dma_buf() - allocate a dma-buf buffer for other importers
 * @request: buffer allocation request struct.
 *
 */
int armchina_aipu_alloc_dma_buf(struct aipu_dma_buf_request *request)
{
	if (!aipu_is_ready(aipu))
		return -ENODEV;
	return aipu_alloc_dma_buf(&aipu->mm, request);
}
EXPORT_SYMBOL(armchina_aipu_alloc_dma_buf);

/**
 * @armchina_aipu_free_dma_buf() - free a dma-buf buffer
 * @fd: file descriptor related to a buffer allocated by armchina_aipu_alloc_dma_buf.
 *
 */
int armchina_aipu_free_dma_buf(int fd)
{
	if (!aipu_is_ready(aipu))
		return -ENODEV;
	return aipu_free_dma_buf(&aipu->mm, fd);
}
EXPORT_SYMBOL(armchina_aipu_free_dma_buf);
