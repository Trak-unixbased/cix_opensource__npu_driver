From 7a0dcf478e2e93cf46ea8b3903853bb958b992e9 Mon Sep 17 00:00:00 2001
From: Olivier Pavilla <olivier.pavilla@gmail.com>
Date: Tue, 27 Oct 2026 13:53:52 +0100
Subject: [PATCH] aipu: fix error handling, runtime PM and lifetime bugs; build
 on Linux 7.0

---
 driver/armchina-npu/aipu.c         | 471 +++++++++++++++++------------
 driver/armchina-npu/aipu_dma_buf.c |   4 +
 driver/armchina-npu/aipu_mm.c      |   5 +
 3 files changed, 286 insertions(+), 194 deletions(-)

diff --git a/driver/armchina-npu/aipu.c b/driver/armchina-npu/aipu.c
index a3b0bf9..f0ab855 100644
--- a/driver/armchina-npu/aipu.c
+++ b/driver/armchina-npu/aipu.c
@@ -26,32 +26,67 @@
 #include "cix_sky1_soc.h"
 #endif
 
+/*
+ * Single AIPU instance shared by all partitions/cores. It is allocated with
+ * devm_kzalloc() on the first probed platform device, so it is only valid
+ * while that device is bound: it is published here only once fully
+ * initialised, and cleared on failure and on remove so that a deferred
+ * probe (-EPROBE_DEFER) or an unbind/bind cycle never reuses freed memory.
+ */
 static struct aipu_priv *aipu;
 
+/* /dev/aipu is usable only once the job manager AND partitions exist. */
+static bool aipu_is_ready(struct aipu_priv *priv)
+{
+	return priv && !aipu_priv_check_status(priv) && priv->partitions;
+}
+
 static int aipu_open(struct inode *inode, struct file *filp)
 {
-	filp->private_data = aipu;
+	struct aipu_priv *priv = aipu;
+	int ret;
+
+	/*
+	 * misc_register() runs inside init_aipu_priv(), i.e. before the job
+	 * manager and partitions are ready: refuse early openers.
+	 */
+	if (!aipu_is_ready(priv))
+		return -ENODEV;
 
 #ifdef CONFIG_SKY1
-	sky1_npu_pm_runtime_get_sync(aipu->dev);
+	/* resume_and_get drops the usage count itself on failure */
+	ret = pm_runtime_resume_and_get(priv->dev);
+	if (ret < 0) {
+		dev_err(priv->dev, "open: runtime resume failed: %d\n", ret);
+		return ret;
+	}
 #endif
 
-	return aipu_priv_check_status(aipu);
+	filp->private_data = priv;
+	return 0;
 }
 
 static int aipu_release(struct inode *inode, struct file *filp)
 {
-	int ret = 0;
-	struct aipu_priv *aipu = filp->private_data;
+	int ret;
+	struct aipu_priv *priv = filp->private_data;
+
+	if (!priv)
+		return 0;
 
-	ret = aipu_job_manager_cancel_jobs(&aipu->job_manager, filp);
+	/*
+	 * The VFS ignores the return value of ->release(): never bail out
+	 * early, otherwise the buffers and the runtime PM reference of this
+	 * file are leaked forever (NPU never suspends again).
+	 */
+	ret = aipu_job_manager_cancel_jobs(&priv->job_manager, filp);
 	if (ret)
-		return ret;
+		dev_warn(priv->dev, "release: cancel jobs failed: %d\n", ret);
 
-	aipu_mm_free_buffers(&aipu->mm, filp);
+	aipu_mm_free_buffers(&priv->mm, filp);
 
 #ifdef CONFIG_SKY1
-	sky1_npu_pm_runtime_put(aipu->dev);
+	pm_runtime_put(priv->dev);
 #endif
 
 	return 0;
@@ -62,6 +97,7 @@ static long aipu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
 	int ret = 0;
 	struct aipu_priv *aipu = filp->private_data;
 	struct aipu_job_manager *manager = &aipu->job_manager;
+	void __user *uarg = (void __user *)arg;
 
 	u32 partition_cnt = 0;
 	struct aipu_partition_cap *partition_cap = NULL;
@@ -79,60 +115,65 @@ static long aipu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
 	struct aipu_cluster_status cluster_status;
 	struct aipu_running_job_query running_job_query;
 	int fd = 0;
+	int id = 0;
 
 	u64 job_id;
 	struct aipu_rebind_buf_desc rebind_buf;
 	struct aipu_bind_buf_desc bind_buf;
 
+	/*
+	 * Convention: a failed copy_{from,to}_user() returns -EFAULT, never
+	 * the raw "bytes not copied" count (positive value seen as success by
+	 * the UMD) nor a silent 0.
+	 */
 	switch (cmd) {
 	case AIPU_IOCTL_QUERY_CAP:
 		ret = aipu_priv_query_capability(aipu, &cap);
-		if (!ret && copy_to_user((struct aipu_cap __user *)arg, &cap, sizeof(cap)))
-			ret = -EINVAL;
+		if (!ret && copy_to_user(uarg, &cap, sizeof(cap)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_QUERY_PARTITION_CAP:
 		partition_cnt = aipu_priv_get_partition_cnt(aipu);
+		if (!partition_cnt) {
+			ret = -ENODEV;
+			break;
+		}
 		partition_cap = kcalloc(partition_cnt, sizeof(*partition_cap), GFP_KERNEL);
-		if (partition_cap) {
-			ret = aipu_priv_query_partition_capability(aipu, partition_cap);
-			if (!ret &&
-			    copy_to_user((struct aipu_partition_cap __user *)arg, partition_cap,
-					 partition_cnt * sizeof(*partition_cap)))
-				ret = -EINVAL;
-			kfree(partition_cap);
-			partition_cap = NULL;
-		} else {
+		if (!partition_cap) {
 			ret = -ENOMEM;
+			break;
 		}
+		ret = aipu_priv_query_partition_capability(aipu, partition_cap);
+		if (!ret && copy_to_user(uarg, partition_cap,
+					 partition_cnt * sizeof(*partition_cap)))
+			ret = -EFAULT;
+		kfree(partition_cap);
 		break;
 	case AIPU_IOCTL_REQ_BUF:
-		if (!copy_from_user(&buf_req, (struct aipu_buf_request __user *)arg,
-				    sizeof(buf_req))) {
-			if (!aipu->mm.has_iommu)
-				buf_req.alloc_mode = AIPU_DMA_BUF_MALLOC_DEFAULT;
-
-			if (buf_req.alloc_mode == AIPU_DMA_BUF_MALLOC_IOVA ||
-			    buf_req.alloc_mode == AIPU_DMA_BUF_MALLOC_IOVA_PHY) {
-			    ret = aipu_job_manager_alloc_exec_id(manager, &buf_req.exec_id);
-			    if (ret)
-			    	return ret;
-			} else {
-				buf_req.exec_id = 0;
-			}
-			ret = aipu_alloc_dma_iova_phy(&aipu->mm, &buf_req, filp);
-			if (!ret &&
-			    copy_to_user((struct aipu_buf_request __user *)arg, &buf_req,
-					 sizeof(buf_req)))
-				ret = -EINVAL;
+		if (copy_from_user(&buf_req, uarg, sizeof(buf_req))) {
+			ret = -EFAULT;
+			break;
+		}
+		if (!aipu->mm.has_iommu)
+			buf_req.alloc_mode = AIPU_DMA_BUF_MALLOC_DEFAULT;
+
+		if (buf_req.alloc_mode == AIPU_DMA_BUF_MALLOC_IOVA ||
+		    buf_req.alloc_mode == AIPU_DMA_BUF_MALLOC_IOVA_PHY) {
+			ret = aipu_job_manager_alloc_exec_id(manager, &buf_req.exec_id);
+			if (ret)
+				break;
 		} else {
-			ret = -EINVAL;
+			buf_req.exec_id = 0;
 		}
+		ret = aipu_alloc_dma_iova_phy(&aipu->mm, &buf_req, filp);
+		if (!ret && copy_to_user(uarg, &buf_req, sizeof(buf_req)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_FREE_BUF:
-		if (!copy_from_user(&desc, (struct buf_desc __user *)arg, sizeof(desc)))
+		if (!copy_from_user(&desc, uarg, sizeof(desc)))
 			ret = aipu_free_dma_iova_phy(&aipu->mm, &desc, filp);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_DISABLE_SRAM:
 		ret = aipu_mm_disable_sram_allocation(&aipu->mm, filp);
@@ -141,43 +182,40 @@ static long aipu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
 		ret = aipu_mm_enable_sram_allocation(&aipu->mm, filp);
 		break;
 	case AIPU_IOCTL_SCHEDULE_JOB:
-		if (!copy_from_user(&user_job, (struct user_job_desc __user *)arg,
-				    sizeof(user_job)))
+		if (!copy_from_user(&user_job, uarg, sizeof(user_job)))
 			ret = aipu_job_manager_scheduler(manager, &user_job, filp);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_QUERY_STATUS:
-		if (!copy_from_user(&status, (struct job_status_query __user *)arg,
-				    sizeof(status))) {
-			ret = aipu_job_manager_get_job_status(manager, &status, filp);
-			if (!ret &&
-			    copy_to_user((struct job_status_query __user *)arg, &status,
-					 sizeof(status)))
-				ret = -EINVAL;
+		/* original code returned 0 (success) when copy_from_user failed */
+		if (copy_from_user(&status, uarg, sizeof(status))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_job_manager_get_job_status(manager, &status, filp);
+		if (!ret && copy_to_user(uarg, &status, sizeof(status)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_KILL_TIMEOUT_JOB:
-		if (!copy_from_user(&job_id, (u64 __user *)arg, sizeof(job_id)))
+		if (!copy_from_user(&job_id, uarg, sizeof(job_id)))
 			ret = aipu_job_manager_invalidate_timeout_job(manager, job_id);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_REQ_IO:
-		if (!copy_from_user(&io_req, (struct aipu_io_req __user *)arg, sizeof(io_req))) {
-			ret = aipu_priv_io_rw(aipu, &io_req);
-			if (!ret &&
-			    copy_to_user((struct aipu_io_req __user *)arg, &io_req,
-					 sizeof(io_req)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&io_req, uarg, sizeof(io_req))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_priv_io_rw(aipu, &io_req);
+		if (!ret && copy_to_user(uarg, &io_req, sizeof(io_req)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_GET_HW_STATUS:
 		ret = aipu_job_manager_get_hw_status(manager, &hw);
-		if (!ret && copy_to_user((struct aipu_hw_status __user *)arg, &hw, sizeof(hw)))
-			ret = -EINVAL;
+		if (!ret && copy_to_user(uarg, &hw, sizeof(hw)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_ABORT_CMD_POOL:
 		ret = aipu_job_manager_abort_cmd_pool(manager);
@@ -189,158 +227,148 @@ static long aipu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
 		ret = aipu_job_manager_enable_tick_counter(manager);
 		break;
 	case AIPU_IOCTL_CONFIG_CLUSTERS:
-		if (!copy_from_user(&config_clusters, (struct aipu_config_clusters __user *)arg,
-				    sizeof(config_clusters)))
+		if (!copy_from_user(&config_clusters, uarg, sizeof(config_clusters)))
 			ret = aipu_job_manager_config_clusters(manager, &config_clusters);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_ALLOC_DMA_BUF:
-		if (!copy_from_user(&dmabuf_req, (struct aipu_dma_buf_request __user *)arg,
-				    sizeof(dmabuf_req))) {
-			ret = aipu_alloc_dma_buf(&aipu->mm, &dmabuf_req);
-			if (!ret && copy_to_user((struct aipu_dma_buf_request __user *)arg,
-						 &dmabuf_req, sizeof(dmabuf_req)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&dmabuf_req, uarg, sizeof(dmabuf_req))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_alloc_dma_buf(&aipu->mm, &dmabuf_req);
+		if (!ret && copy_to_user(uarg, &dmabuf_req, sizeof(dmabuf_req)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_FREE_DMA_BUF:
-		if (!copy_from_user(&fd, (int __user *)arg, sizeof(fd)))
+		if (!copy_from_user(&fd, uarg, sizeof(fd)))
 			ret = aipu_free_dma_buf(&aipu->mm, fd);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_GET_DMA_BUF_INFO:
-		if (!copy_from_user(&dmabuf_info, (struct aipu_dma_buf __user *)arg,
-				    sizeof(dmabuf_info))) {
-			ret = aipu_get_dma_buf_info(&dmabuf_info);
-			if (!ret && copy_to_user((struct aipu_dma_buf __user *)arg,
-						 &dmabuf_info, sizeof(dmabuf_info)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&dmabuf_info, uarg, sizeof(dmabuf_info))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_get_dma_buf_info(&dmabuf_info);
+		if (!ret && copy_to_user(uarg, &dmabuf_info, sizeof(dmabuf_info)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_ATTACH_DMA_BUF:
-		if (!copy_from_user(&dmabuf_info, (struct aipu_dma_buf __user *)arg,
-				    sizeof(dmabuf_info))) {
-			ret = aipu_attach_dma_buf(&aipu->mm, &dmabuf_info);
-			if (!ret && copy_to_user((struct aipu_dma_buf __user *)arg,
-						 &dmabuf_info, sizeof(dmabuf_info)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&dmabuf_info, uarg, sizeof(dmabuf_info))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_attach_dma_buf(&aipu->mm, &dmabuf_info);
+		if (!ret && copy_to_user(uarg, &dmabuf_info, sizeof(dmabuf_info)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_DETACH_DMA_BUF:
-		if (!copy_from_user(&fd, (int __user *)arg, sizeof(fd)))
+		if (!copy_from_user(&fd, uarg, sizeof(fd)))
 			ret = aipu_detach_dma_buf(&aipu->mm, fd);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_GET_DRIVER_VERSION:
-		ret = copy_to_user((char __user *)arg, KMD_VERSION, sizeof(KMD_VERSION));
+		/* copy_to_user() returns the number of bytes NOT copied */
+		if (copy_to_user(uarg, KMD_VERSION, sizeof(KMD_VERSION)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_ALLOC_GRID_ID:
-		ret = aipu_job_manager_alloc_grid_id(manager);
-		if (ret >= 0)
-			ret = copy_to_user((char __user *)arg, &ret, sizeof(ret));
+		/*
+		 * Original code reused 'ret' both as the ID and as the copy
+		 * status, so a successful call returned 0 and a partial copy
+		 * returned a positive byte count.
+		 */
+		id = aipu_job_manager_alloc_grid_id(manager);
+		if (id < 0)
+			ret = id;
+		else if (copy_to_user(uarg, &id, sizeof(id)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_ALLOC_GROUP_ID:
-		if (!copy_from_user(&id_desc, (struct aipu_id_desc __user *)arg,
-				    sizeof(id_desc))) {
-			ret = aipu_job_manager_alloc_group_id(manager, &id_desc);
-			if (!ret && copy_to_user((char __user *)arg, &id_desc,
-						 sizeof(id_desc)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&id_desc, uarg, sizeof(id_desc))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_job_manager_alloc_group_id(manager, &id_desc);
+		if (!ret && copy_to_user(uarg, &id_desc, sizeof(id_desc)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_FREE_GROUP_ID:
-		if (!copy_from_user(&id_desc, (struct aipu_id_desc __user *)arg,
-				    sizeof(id_desc)))
+		if (!copy_from_user(&id_desc, uarg, sizeof(id_desc)))
 			ret = aipu_job_manager_free_group_id(manager, &id_desc);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_GET_CLUSTER_STATUS:
-		if (!copy_from_user(&cluster_status, (struct aipu_cluster_status __user *)arg,
-				    sizeof(cluster_status))) {
-			if (aipu && aipu->ops && aipu->ops->get_partition_status) {
-				ret = aipu->ops->get_partition_status(aipu, &cluster_status);
-				if (!ret && copy_to_user((char __user *)arg, &cluster_status,
-							 sizeof(cluster_status)))
-					ret = -EINVAL;
-			} else {
-				ret = -EINVAL;
-			}
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&cluster_status, uarg, sizeof(cluster_status))) {
+			ret = -EFAULT;
+			break;
 		}
+		if (!aipu->ops || !aipu->ops->get_partition_status) {
+			ret = -EOPNOTSUPP;
+			break;
+		}
+		ret = aipu->ops->get_partition_status(aipu, &cluster_status);
+		if (!ret && copy_to_user(uarg, &cluster_status, sizeof(cluster_status)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_GET_RUNNING_JOB_THREAD_ID:
-		if (!copy_from_user(&running_job_query, (struct aipu_running_job_query __user *)arg,
-				    sizeof(running_job_query))) {
-			ret = aipu_job_manager_get_running_job_thread(manager, &running_job_query);
-			if (!ret && copy_to_user((char __user *)arg, &running_job_query,
-						 sizeof(running_job_query)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&running_job_query, uarg, sizeof(running_job_query))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_job_manager_get_running_job_thread(manager, &running_job_query);
+		if (!ret && copy_to_user(uarg, &running_job_query, sizeof(running_job_query)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_ALLOC_SFLAG_ID:
-		if (!copy_from_user(&id_desc, (struct aipu_id_desc __user *)arg,
-				    sizeof(id_desc))) {
-			ret = aipu_job_manager_alloc_sflag_id(manager, &id_desc);
-			if (!ret && copy_to_user((char __user *)arg, &id_desc,
-						 sizeof(id_desc)))
-				ret = -EINVAL;
-		} else {
-			ret = -EINVAL;
+		if (copy_from_user(&id_desc, uarg, sizeof(id_desc))) {
+			ret = -EFAULT;
+			break;
 		}
+		ret = aipu_job_manager_alloc_sflag_id(manager, &id_desc);
+		if (!ret && copy_to_user(uarg, &id_desc, sizeof(id_desc)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_FREE_SFLAG_ID:
-		if (!copy_from_user(&id_desc, (struct aipu_id_desc __user *)arg,
-				    sizeof(id_desc)))
+		if (!copy_from_user(&id_desc, uarg, sizeof(id_desc)))
 			ret = aipu_job_manager_free_sflag_id(manager, &id_desc);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_REBIND_DMA_BUF:
-		if (!copy_from_user(&rebind_buf, (struct buf_desc __user *)arg, sizeof(rebind_buf))) {
-			ret = aipu_rebind_dma_iova_phy(&aipu->mm, &rebind_buf, filp);
-			if (!ret &&
-			    copy_to_user((struct aipu_buf_request __user *)arg, &rebind_buf,
-					 sizeof(rebind_buf)))
-				ret = -EINVAL;
-		} else
-			ret = -EINVAL;
+		if (copy_from_user(&rebind_buf, uarg, sizeof(rebind_buf))) {
+			ret = -EFAULT;
+			break;
+		}
+		ret = aipu_rebind_dma_iova_phy(&aipu->mm, &rebind_buf, filp);
+		if (!ret && copy_to_user(uarg, &rebind_buf, sizeof(rebind_buf)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_BIND_DMA_BUF:
-		if (!copy_from_user(&bind_buf, (struct buf_desc __user *)arg, sizeof(bind_buf))) {
-			ret = aipu_bind_dma_iova_phy(&aipu->mm, &bind_buf, filp);
-			if (!ret &&
-			    copy_to_user((struct aipu_buf_request __user *)arg, &bind_buf,
-					 sizeof(bind_buf)))
-				ret = -EINVAL;
-		} else
-			ret = -EINVAL;
+		if (copy_from_user(&bind_buf, uarg, sizeof(bind_buf))) {
+			ret = -EFAULT;
+			break;
+		}
+		ret = aipu_bind_dma_iova_phy(&aipu->mm, &bind_buf, filp);
+		if (!ret && copy_to_user(uarg, &bind_buf, sizeof(bind_buf)))
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_BUF_CACHE_INVALID:
-		if (!copy_from_user(&desc, (struct buf_desc __user *)arg, sizeof(desc)))
+		if (!copy_from_user(&desc, uarg, sizeof(desc)))
 			ret = aipu_mm_cache_invalid(&aipu->mm, &desc);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	case AIPU_IOCTL_BUF_CACHE_FLUSH:
-		if (!copy_from_user(&desc, (struct buf_desc __user *)arg, sizeof(desc)))
+		if (!copy_from_user(&desc, uarg, sizeof(desc)))
 			ret = aipu_mm_cache_flush(&aipu->mm, &desc);
 		else
-			ret = -EINVAL;
+			ret = -EFAULT;
 		break;
 	default:
 		ret = -ENOTTY;
@@ -366,13 +394,13 @@ static int aipu_mmap(struct file *filp, struct vm_area_struct *vma)
 	return aipu_mm_mmap_buf(&aipu->mm, vma, filp);
 }
 
-static unsigned int aipu_poll(struct file *filp, struct poll_table_struct *wait)
+static __poll_t aipu_poll(struct file *filp, struct poll_table_struct *wait)
 {
-	unsigned int mask = 0;
+	__poll_t mask = 0;
 	struct aipu_priv *aipu = filp->private_data;
 
 	if (aipu_job_manager_has_end_job(&aipu->job_manager, filp, wait, task_pid_nr(current)))
-		mask |= POLLIN | POLLRDNORM;
+		mask |= EPOLLIN | EPOLLRDNORM;
 
 	return mask;
 }
@@ -410,36 +438,52 @@ int armchina_aipu_probe(struct platform_device *p_dev, struct aipu_soc *soc,
 	int ret = 0;
 	struct device *dev = &p_dev->dev;
 	struct aipu_partition *partition = NULL;
+	struct aipu_priv *priv = aipu;
 	u32 id = 0;
 
 	/* create & init aipu priv struct shared by all clusters/cores */
-	if (!aipu) {
-		aipu = devm_kzalloc(dev, sizeof(*aipu), GFP_KERNEL);
-		if (!aipu)
+	if (!priv) {
+		priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
+		if (!priv)
 			return -ENOMEM;
 
 		dev_info(dev, "AIPU KMD (v%s) probe start...\n", KMD_VERSION);
 
-		platform_set_drvdata(p_dev, aipu);
-
-		ret = init_aipu_priv(aipu, p_dev, &aipu_fops, soc, ops);
-		if (ret)
+		platform_set_drvdata(p_dev, priv);
+
+		/*
+		 * Publish the pointer before init: misc_register() happens
+		 * inside, and aipu_open() relies on is_init/partitions checks
+		 * (aipu_is_ready) to reject early openers.
+		 */
+		aipu = priv;
+		ret = init_aipu_priv(priv, p_dev, &aipu_fops, soc, ops);
+		if (ret) {
+			/* devm memory is released when probe fails: forget it */
+			aipu = NULL;
 			return ret;
+		}
 	}
 
 	device_property_read_u32(dev, "core-id", &id);
 
-	WARN_ON(!aipu->ops);
-	partition = aipu->ops->create_partitions(aipu, id, p_dev);
-	if (IS_ERR(partition))
+	if (WARN_ON(!priv->ops || !priv->ops->create_partitions)) {
+		ret = -EINVAL;
+		goto out_clean;
+	}
+
+	partition = priv->ops->create_partitions(priv, id, p_dev);
+	if (IS_ERR_OR_NULL(partition)) {
+		/* original code returned 0 here: probe "succeeded" on a dead NPU */
+		ret = partition ? PTR_ERR(partition) : -ENODEV;
+		dev_err(dev, "create partitions failed: %d\n", ret);
 		goto out_clean;
+	}
 
-	goto finish;
+	return 0;
 
 out_clean:
 	armchina_aipu_remove(p_dev);
-
-finish:
 	return ret;
 }
 EXPORT_SYMBOL(armchina_aipu_probe);
@@ -456,12 +500,39 @@ EXPORT_SYMBOL(armchina_aipu_probe);
  */
 int armchina_aipu_remove(struct platform_device *p_dev)
 {
-	if (!aipu || !aipu->is_init)
+	struct aipu_priv *priv = aipu;
+	int ret = 0;
+
+	if (!priv)
 		return 0;
-	return deinit_aipu_priv(aipu);
+
+	if (priv->is_init)
+		ret = deinit_aipu_priv(priv);
+
+	/* priv is devm-managed and about to be freed: drop the global ref */
+	aipu = NULL;
+	return ret;
 }
 EXPORT_SYMBOL(armchina_aipu_remove);
 
+/* Common guard for the PM callbacks; returns NULL when nothing to do. */
+static struct aipu_job_manager *aipu_pm_manager(const char *who)
+{
+	struct aipu_job_manager *manager;
+
+	/* original code dereferenced aipu->partitions before any NULL check */
+	if (!aipu || !aipu->is_init || !aipu->partitions) {
+		pr_debug("aipu %s: npu driver is not ready.\n", who);
+		return NULL;
+	}
+
+	manager = get_job_manager(aipu->partitions);
+	if (!manager)
+		pr_debug("aipu %s: npu driver is not ready.\n", who);
+
+	return manager;
+}
+
 /**
  * @armchina_aipu_suspend() - suspend operation for platfom driver provided by ArmChina
  * @p_dev: pointer to the AIPU platform device struct.
@@ -473,18 +544,19 @@ EXPORT_SYMBOL(armchina_aipu_remove);
  */
 int armchina_aipu_suspend(struct platform_device *p_dev, pm_message_t state)
 {
-	int ret = 0;
-	struct aipu_job_manager *manager = get_job_manager(aipu->partitions);
+	int ret;
+	struct aipu_job_manager *manager = aipu_pm_manager("suspend");
 
-	if (!manager) {
-		pr_err("susppend: npu driver is not ready.\n");
+	if (!manager)
 		return 0;
-	}
 
-	ret = aipu_job_manager_suspend(get_job_manager(aipu->partitions));
+	/* quiesce the NPU first (registers need the clock), then gate it */
+	ret = aipu_job_manager_suspend(manager);
+	if (ret)
+		return ret;
 
-	if (ret == 0 && aipu && aipu->soc_ops && aipu->soc_ops->disable_clk)
-		aipu->soc_ops->disable_clk(aipu->partitions->dev, aipu->soc);
+	if (aipu->soc_ops && aipu->soc_ops->disable_clk)
+		ret = aipu->soc_ops->disable_clk(aipu->partitions->dev, aipu->soc);
 
 	return ret;
 }
@@ -501,18 +573,25 @@ EXPORT_SYMBOL(armchina_aipu_suspend);
  */
 int armchina_aipu_resume(struct platform_device *p_dev)
 {
-	int ret = 0;
-	struct aipu_job_manager *manager = get_job_manager(aipu->partitions);
+	int ret;
+	struct aipu_job_manager *manager = aipu_pm_manager("resume");
 
-	if (!manager) {
-		pr_err("resume: npu driver is not ready.\n");
+	if (!manager)
 		return 0;
-	}
 
-	ret = aipu_job_manager_resume(get_job_manager(aipu->partitions));
+	/*
+	 * Original order was resume (register writes on V3) THEN enable_clk:
+	 * touching an unclocked IP on arm64 typically ends in an SError.
+	 */
+	if (aipu->soc_ops && aipu->soc_ops->enable_clk) {
+		ret = aipu->soc_ops->enable_clk(aipu->partitions->dev, aipu->soc);
+		if (ret)
+			return ret;
+	}
 
-	if (ret == 0 && aipu && aipu->soc_ops && aipu->soc_ops->enable_clk)
-		aipu->soc_ops->enable_clk(aipu->partitions->dev, aipu->soc);
+	ret = aipu_job_manager_resume(manager);
+	if (ret && aipu->soc_ops && aipu->soc_ops->disable_clk)
+		aipu->soc_ops->disable_clk(aipu->partitions->dev, aipu->soc);
 
 	return ret;
 }
@@ -525,6 +604,8 @@ EXPORT_SYMBOL(armchina_aipu_resume);
  */
 int armchina_aipu_alloc_dma_buf(struct aipu_dma_buf_request *request)
 {
+	if (!aipu_is_ready(aipu))
+		return -ENODEV;
 	return aipu_alloc_dma_buf(&aipu->mm, request);
 }
 EXPORT_SYMBOL(armchina_aipu_alloc_dma_buf);
@@ -536,6 +617,8 @@ EXPORT_SYMBOL(armchina_aipu_alloc_dma_buf);
  */
 int armchina_aipu_free_dma_buf(int fd)
 {
+	if (!aipu_is_ready(aipu))
+		return -ENODEV;
 	return aipu_free_dma_buf(&aipu->mm, fd);
 }
 EXPORT_SYMBOL(armchina_aipu_free_dma_buf);
diff --git a/driver/armchina-npu/aipu_dma_buf.c b/driver/armchina-npu/aipu_dma_buf.c
index 3e85001..3bf6769 100755
--- a/driver/armchina-npu/aipu_dma_buf.c
+++ b/driver/armchina-npu/aipu_dma_buf.c
@@ -381,5 +381,9 @@ int aipu_detach_dma_buf(struct aipu_memory_manager *mm, int fd)
 }
 
 #if KERNEL_VERSION(5, 4, 0) < LINUX_VERSION_CODE
+#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
+MODULE_IMPORT_NS("DMA_BUF");
+#else
 MODULE_IMPORT_NS(DMA_BUF);
 #endif
+#endif
diff --git a/driver/armchina-npu/aipu_mm.c b/driver/armchina-npu/aipu_mm.c
index 90a4e52..12c3805 100755
--- a/driver/armchina-npu/aipu_mm.c
+++ b/driver/armchina-npu/aipu_mm.c
@@ -1,6 +1,11 @@
 // SPDX-License-Identifier: GPL-2.0
 /* Copyright (c) 2023-2025 Arm Technology (China) Co. Ltd. */
 
+#include <linux/mmzone.h>
+/* MAX_ORDER was renamed MAX_PAGE_ORDER in v6.8 (same inclusive meaning since v6.4) */
+#ifndef MAX_ORDER
+#define MAX_ORDER MAX_PAGE_ORDER
+#endif
 #include <linux/types.h>
 #include <asm/cacheflush.h>
 #include <linux/mm.h>
-- 
2.43.0
