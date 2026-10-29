/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved. */
#ifndef __CIX_SKY1_SOC_H__
#define __CIX_SKY1_SOC_H__

#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include "aipu_priv.h"

#define CIX_NPU_PD_MAX_NUM				(3)

/**
 * struct cix_aipu_priv - SKY1 SoC private data
 * @pd_core:         per NPU core power domain device: genpd virtual device (DT)
 *                   or ACPI "CREx" platform device (ACPI, reference held)
 * @pd_link:         device links NPU -> pd_core (DT only)
 * @pd_cnt:          number of valid entries in @pd_core
 * @opp_pmdomain:    performance domain (DVFS, CIX vendor kernel only)
 * @opp_dl:          device link to @opp_pmdomain
 * @devfreq_profile: devfreq profile
 * @devfreq:         devfreq instance
 */
struct cix_aipu_priv {
	struct device *pd_core[CIX_NPU_PD_MAX_NUM];
	struct device_link *pd_link[CIX_NPU_PD_MAX_NUM];
	int pd_cnt;
	struct device *opp_pmdomain;
	struct device_link *opp_dl;
	struct devfreq_dev_profile devfreq_profile;
	struct devfreq *devfreq;
};

int sky1_npu_pm_runtime_get_sync(struct device *dev);
int sky1_npu_pm_runtime_put(struct device *dev);

#endif /* __CIX_SKY1_SOC_H__ */
