// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023-2025 Arm Technology (China) Co. Ltd. */

#include <linux/platform_device.h>
#include <linux/of.h>
#include "aipu_priv.h"
#include "aipu_partition.h"
#include "aipu_common.h"
#include "zhouyi.h"
#include "v3.h"

static int init_aipu_partition(struct aipu_partition *partition, u32 *clusters, int tot_cluster,
			       struct platform_device *p_dev)
{
	int ret = 0;
	u32 cluster_cnt = 0;
	u32 iter = 0;
	u32 val = 0;

	if (!partition || !clusters || !p_dev || !tot_cluster)
		return -EINVAL;

	WARN_ON(!partition->priv);

	partition->arch = AIPU_ARCH_ZHOUYI;
	partition->dev = &p_dev->dev;
	partition->reg = &partition->priv->reg;
	partition->irq_obj = partition->priv->irq_obj;
	mutex_init(&partition->reset_lock);
	mutex_init(&partition->page_lock);
	partition->ops = get_zhouyi_v3_ops();

	/* unused fields */
	partition->reg_attr = NULL;
	partition->clk_attr = NULL;
	partition->disable_attr = NULL;
	partition->config = 0;
	partition->max_sched_num = 0;
	partition->dtcm_base = 0;
	partition->dtcm_size = 0;
	atomic_set(&partition->disable, 0);

	/* get clusters of this partition */
	for (iter = 0; iter < tot_cluster; iter++) {
		if (partition->id == clusters[2 * iter + 1]) {
			/* partition->clusters[] has ARRAY_SIZE() entries (UBSAN/overflow) */
			if (cluster_cnt >= ARRAY_SIZE(partition->clusters)) {
				dev_err(partition->dev, "too many clusters in partition %d\n",
					partition->id);
				return -EINVAL;
			}
			cluster_cnt++;
			partition->clusters[cluster_cnt - 1].id = clusters[2 * iter];
			val = aipu_read32(partition->reg, CLUSTER_CONFIG_REG(clusters[2 * iter]));
			partition->clusters[cluster_cnt - 1].core_cnt = GET_AIPU_CORE_NUM(val);
			atomic_set(&partition->clusters[cluster_cnt - 1].en_core_cnt,
				   GET_AIPU_CORE_NUM(val));
			partition->clusters[cluster_cnt - 1].tec_cnt = GET_TEC_NUM(val);

			aipu_write32(partition->reg, DEBUG_PAGE_SELECTION_REG,
				     SELECT_DEBUG_CORE(0, 0));
			partition->clusters[cluster_cnt - 1].gm_bytes =
				get_gm_size(aipu_read32(partition->reg, DEBUG_CLUSTER_GM_CONTROL));
			aipu_write32(partition->reg, DEBUG_PAGE_SELECTION_REG, DISABLE_DEBUG);
			/*
			 * GM is optional (gm-policy = 0): the -EINVAL returned then
			 * used to become the partition init result and fail probe.
			 */
			if (aipu_mm_init_gm(&partition->priv->mm,
					    partition->clusters[cluster_cnt - 1].gm_bytes))
				dev_dbg(partition->dev, "GM not used (policy none)\n");
		}
	}

	partition->cluster_cnt = cluster_cnt;
	partition->ops->initialize(partition);

#ifdef CONFIG_SYSFS
	if (IS_ERR(aipu_common_create_attr(partition->dev, &partition->reg_attr, "ext_registers",
					   0644, aipu_common_ext_register_sysfs_show,
					   aipu_common_ext_register_sysfs_store)))
		dev_err(partition->dev, "[init_partition] init sysfs <ext_registers> failed");
#endif

	partition->is_init = true;
	return ret;
}

static struct aipu_partition *v3_create_partitions(struct aipu_priv *aipu,
						   int id, struct platform_device *p_dev)
{
	int ret = 0;
	int version = 0;
	int config = 0;
	struct aipu_partition *partitions = NULL;
	u32 *cluster_arr = NULL;
	u32 cluster_cnt = 0;
	u32 partition_cnt = 1;
	u32 iter = 0;
	u32 build_info = 0;

	if (!aipu || !p_dev)
		return ERR_PTR(-EINVAL);

	zhouyi_detect_aipu_version(p_dev, &version, &config, NULL);
	dev_info(&p_dev->dev, "AIPU detected: zhouyi-v3\n");

	WARN_ON(!aipu->is_init);

	/* Get cluster count (by default 1) */
	/*
	 * Cluster-partition attribute should be in the following format:
	 *	cluster-partition = <cluster_id0 partition_idx>, <cluster_id1 partition_idy>,
	 *			    <cluster_id2 partition_idz>, ...;
	 * Both ids of clusters and partitions should be u32 numbered as 0, 1, 2, 3, ...
	 * One cluster should only be within one partition.
	 */
	ret = device_property_count_u32(&p_dev->dev, "cluster-partition");
	if (ret > 0 && ret < 2) {
		dev_err(&p_dev->dev, "check your dts or acpi table: bad cluster-partition");
		return ERR_PTR(-EINVAL);
	}

	cluster_cnt = ret > 0 ? ret >> 1 : 1;
	if (cluster_cnt > MAX_CLUSTER_NUM) {
		dev_err(&p_dev->dev, "%u clusters declared, at most %d supported\n",
			cluster_cnt, MAX_CLUSTER_NUM);
		return ERR_PTR(-EINVAL);
	}

	cluster_arr = devm_kcalloc(&p_dev->dev, cluster_cnt * 2, sizeof(u32), GFP_KERNEL);
	if (!cluster_arr)
		return ERR_PTR(-ENOMEM);

	if (ret <= 0) {
		/*
		 * The property is optional: the old code announced a default
		 * config and then failed the probe reading the missing array.
		 * Default: cluster 0 in partition 0.
		 */
		dev_warn(&p_dev->dev, "use the default config (1 cluster)");
		cluster_arr[0] = 0;
		cluster_arr[1] = 0;
	} else if (device_property_read_u32_array(&p_dev->dev, "cluster-partition", cluster_arr,
						  cluster_cnt * 2)) {
		dev_err(&p_dev->dev, "check your dts or acpi table: read cluster-partition failed");
		devm_kfree(&p_dev->dev, cluster_arr);
		return ERR_PTR(-EINVAL);
	}

	for (iter = 0; iter < cluster_cnt; iter++) {
		if (cluster_arr[2 * iter + 1] > (partition_cnt - 1))
			partition_cnt = cluster_arr[2 * iter + 1] + 1;
	}

	partitions = devm_kcalloc(&p_dev->dev, partition_cnt, sizeof(*partitions), GFP_KERNEL);
	if (!partitions) {
		devm_kfree(&p_dev->dev, cluster_arr);
		return ERR_PTR(-ENOMEM);
	}

	/* register base and interrupt are shared among all partitions */
	/* do this before calling init_aipu_partition() */
	ret = aipu_common_init_reg_irq(p_dev, &partitions[0], &aipu->reg, &aipu->irq_obj);
	if (ret) {
		devm_kfree(&p_dev->dev, partitions);
		partitions = ERR_PTR(ret);
		goto finish;
	}

	/* global soft-reset before per partition config */
	ret = aipu->ops->global_soft_reset(aipu);
	if (ret) {
		dev_err(&p_dev->dev, "[init partition] global soft reset failed");
		/* the IRQ and the register mapping used to be leaked here */
		goto init_partition_fail;
	}

	build_info = aipu_read32(&aipu->reg, TSM_BUILD_INFO_REG);
	aipu->max_partition_cnt = GET_MAX_PARTITION_NUM(build_info);
	aipu->max_cmd_pool_cnt = GET_MAX_CMD_POOL_NUM(build_info);

	WARN_ON(partition_cnt > aipu->max_partition_cnt);
	WARN_ON(partition_cnt > aipu->max_cmd_pool_cnt);
	WARN_ON(cluster_cnt > MAX_CLUSTER_NUM);

	/* check if clusters are all present after reg init */
	for (iter = 0; iter < cluster_cnt; iter++) {
		if (!IS_CLUSTER_PRESENT(aipu_read32(&aipu->reg, CLUSTER_CONFIG_REG(iter)))) {
			dev_err(&p_dev->dev,
				"AIPU cluster #%d was not found but registered in dts\n", iter);
			ret = -EINVAL;
			goto init_partition_fail;
		}
	}

	for (iter = 0; iter < partition_cnt; iter++) {
		partitions[iter].id = iter;
		partitions[iter].priv = aipu;
		partitions[iter].version = version;
		ret = init_aipu_partition(&partitions[iter], cluster_arr, cluster_cnt, p_dev);
		if (ret)
			goto init_partition_fail;
	}

	aipu->partitions = partitions;
	aipu->partition_cnt = partition_cnt;
	aipu->cluster_cnt = cluster_cnt;
	aipu_job_manager_set_partitions_info(&aipu->job_manager, aipu->partition_cnt,
					     aipu->partitions);
	partitions[0].ops->print_hw_id_info(&partitions[0]);
	goto finish;

init_partition_fail:
	for (iter = 0; iter < partition_cnt; iter++)
		aipu_common_destroy_attr(&p_dev->dev, &partitions[iter].reg_attr);
	aipu_destroy_irq_object(aipu->irq_obj);
	aipu->irq_obj = NULL;
	deinit_aipu_ioregion(&aipu->reg);
	aipu->reg.phys = 0;
	aipu->reg.kern = NULL;
	aipu->reg.size = 0;
	devm_kfree(&p_dev->dev, partitions);
	partitions = ERR_PTR(ret);

finish:
	devm_kfree(&p_dev->dev, cluster_arr);
	return partitions;
}

static void v3_destroy_partitions(struct aipu_priv *aipu)
{
	int i = 0;

	if (aipu) {
		if (aipu->irq_obj)
			aipu_destroy_irq_object(aipu->irq_obj);
		aipu->irq_obj = NULL;
		if (aipu->reg.kern)
			deinit_aipu_ioregion(&aipu->reg);
		aipu->reg.kern = NULL;
		aipu->reg.phys = 0;
		aipu->reg.size = 0;
		for (i = 0; aipu->partitions && i < aipu->partition_cnt; i++)
			aipu_common_destroy_attr(aipu->partitions[i].dev,
						 &aipu->partitions[i].reg_attr);
	}
}

static int v3_global_soft_reset(struct aipu_priv *aipu)
{
	return zhouyi_soft_reset(&aipu->reg, TSM_SOFT_RESET_REG, aipu->reset_delay_us);
}

static int zhouyi_v3_read_cluster_status(struct aipu_priv *aipu, struct aipu_cluster_status *status)
{
	int i = 1;

	if (unlikely(!status))
		return -EINVAL;

	if (unlikely(!aipu))
		return -EINVAL;

	mutex_lock(&aipu->partitions[0].page_lock);
	aipu_write32(&aipu->reg, DEBUG_PAGE_SELECTION_REG, SELECT_DEBUG_CORE(0, 0));
	status->cluster_status = aipu_read32(&aipu->reg, DEBUG_CLUSTER_STATUS);
	status->core_status = (aipu_read32(&aipu->reg, DEBUG_CORE_STATUS) >> 4) & 0b1;
	for (; i < aipu->partitions[0].clusters[0].core_cnt; ++i) {
		aipu_write32(&aipu->reg, DEBUG_PAGE_SELECTION_REG, SELECT_DEBUG_CORE(0, i));
		status->core_status |= (((aipu_read32(&aipu->reg, DEBUG_CORE_STATUS) >> 4)
								& 0b1) << i);
	}
	aipu_write32(&aipu->reg, DEBUG_PAGE_SELECTION_REG, DISABLE_DEBUG);
	mutex_unlock(&aipu->partitions[0].page_lock);
	return 0;
}

static struct aipu_priv_operations v3_priv_ops = {
	.create_partitions = v3_create_partitions,
	.destroy_partitions = v3_destroy_partitions,
	.global_soft_reset = v3_global_soft_reset,
	.get_partition_status = zhouyi_v3_read_cluster_status,
};

struct aipu_priv_operations *get_v3_priv_ops(void)
{
	return &v3_priv_ops;
}
