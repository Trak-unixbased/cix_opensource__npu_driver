// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Cix Technology Group Co., Ltd. All Rights Reserved.*/
/**
 * SoC: CIX SKY1 platform
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include <linux/scmi_protocol.h>
#include <linux/debugfs.h>
#include <linux/acpi.h>
#include "armchina_aipu_soc.h"
#include "cix_sky1_soc.h"

#define NPU_CORE_ACPI_NAME_PREFIX       "CRE"

/*
 * dev_pm_domain_attach() takes PD_FLAG_* flags since 6.17; before it took a
 * "bool power_on". Passing "true" to a 7.x kernel means PD_FLAG_NO_DEV_LINK
 * (BIT(0)), i.e. the ACPI core domains were no longer powered on at attach.
 */
#ifdef PD_FLAG_ATTACH_POWER_ON
#define SKY1_PD_ATTACH_FLAGS	(PD_FLAG_ATTACH_POWER_ON | PD_FLAG_DETACH_POWER_OFF)
#else
#define SKY1_PD_ATTACH_FLAGS	true
#endif

static int CIX_NPU_PD_NUM = CIX_NPU_PD_MAX_NUM;

static const char *cix_npu_pd_names[CIX_NPU_PD_MAX_NUM] = {
	"pd_core0", "pd_core1", "pd_core2",
};

static struct aipu_soc sky1 = {
	.priv = NULL,
};

static struct cix_aipu_priv *cix_aipu_priv;

static struct cix_aipu_priv *sky1_priv_init(struct device *dev)
{
	struct cix_aipu_priv *priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);

	if (!priv)
		return ERR_PTR(-ENOMEM);
	return priv;
}

#ifdef CONFIG_ENABLE_DEVFREQ
static void remove_debugfs_dir(const char *name)
{
    struct dentry *dentry;
    struct dentry *parent = debugfs_lookup("opp", NULL);

    if (IS_ERR_OR_NULL(parent)) {
        pr_err("Failed to lookup opp debugfs directory\n");
        return;
    }

    dentry = debugfs_lookup(name, parent);
    if (IS_ERR_OR_NULL(dentry)) {
        pr_err("Failed to lookup %s debugfs directory\n", name);
        dput(parent);
        return;
    }

    debugfs_remove_recursive(dentry);

    dput(dentry);
    dput(parent);
}

static int sky1_npu_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
    struct dev_pm_opp *opp;
    unsigned long pre_freq;
    unsigned long target_freq = *freq;
    int ret;

    opp = devfreq_recommended_opp(dev, freq, flags);
    if (IS_ERR(opp)) {
        dev_err(dev, "Failed to get recommended opp instance\n");
        ret = PTR_ERR(opp);
        return ret;
    }
    dev_pm_opp_put(opp);
    pre_freq = scmi_device_get_freq(cix_aipu_priv->opp_pmdomain);
    ret = scmi_device_set_freq(cix_aipu_priv->opp_pmdomain, *freq);

    dev_dbg(dev, "%s: target=%ld, previous=%ld, current=%ld.",
                    __func__, target_freq, pre_freq, *freq);

    return ret;
}

static int sky1_npu_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
    *freq = scmi_device_get_freq(cix_aipu_priv->opp_pmdomain);
    dev_dbg(dev, "%s: %ld", __func__, *freq);

    return 0;
}

static int sky1_npu_devfreq_get_dev_status(struct device *dev,
                            struct devfreq_dev_status *stat)
{
    dev_dbg(dev, "%s\n", __func__);

    stat->current_frequency = scmi_device_get_freq(cix_aipu_priv->opp_pmdomain);

    return 0;
}

static int sky1_npu_devfreq_init(struct device *dev, struct cix_aipu_priv *cix_aipu_priv)
{
    struct dev_pm_opp *opp;
    struct devfreq_dev_profile *profile;
    unsigned long freq;
    int opp_count;
    int i;
    int ret;

    dev_dbg(dev, "%s\n", __func__);

    profile = &(cix_aipu_priv->devfreq_profile);

#ifdef CONFIG_ARM_SCMI_SUPPORT_DT_ACPI
	cix_aipu_priv->opp_pmdomain = fwnode_dev_pm_domain_attach_by_name(dev, "perf");
#else
	cix_aipu_priv->opp_pmdomain = dev_pm_domain_attach_by_name(dev, "perf");
#endif

    if (IS_ERR_OR_NULL(cix_aipu_priv->opp_pmdomain)) {
        dev_err(dev, "Failed to get perf domain");
        cix_aipu_priv->opp_pmdomain = NULL;
        return -EFAULT;
    }
    cix_aipu_priv->opp_dl = device_link_add(dev, cix_aipu_priv->opp_pmdomain,
                            DL_FLAG_RPM_ACTIVE |
                            DL_FLAG_PM_RUNTIME |
                            DL_FLAG_STATELESS);
    if (IS_ERR_OR_NULL(cix_aipu_priv->opp_dl)) {
        cix_aipu_priv->opp_dl = NULL;
        ret = -ENODEV;
        goto detach_opp;
    }

    /* Add opps to opp power domain. */
    ret = scmi_device_opp_table_parse(cix_aipu_priv->opp_pmdomain);
    if (ret) {
        dev_err(dev, "Failed to add opps to the device");
        ret = -ENODEV;
        goto unlink_opp;
    }
    opp_count = dev_pm_opp_get_opp_count(cix_aipu_priv->opp_pmdomain);
    if (opp_count <= 0) {
        dev_err(dev, "Failed to get opps count.");
        ret = -EINVAL;
        goto unlink_opp;
    }
    profile->freq_table = kmalloc_array(opp_count, sizeof(unsigned long), GFP_KERNEL);
    if (!profile->freq_table) {
        ret = -ENOMEM;
        goto unlink_opp;
    }
    for (i = 0, freq = 0; i < opp_count; i++, freq++) {
        opp = dev_pm_opp_find_freq_ceil(cix_aipu_priv->opp_pmdomain, &freq);
        if (IS_ERR(opp))
            break;
        dev_pm_opp_put(opp);
        profile->freq_table[i] = freq;

        /* Add opps to dev, since register devfreq device as dev */
        ret = dev_pm_opp_add(dev, freq, 0);
        if (ret) {
            dev_err(dev, "Failed to add opp %lu Hz", freq);
            while (i-- > 0) {
                dev_pm_opp_remove(dev, profile->freq_table[i]);
            }
            ret = -ENODEV;
            goto free_table;
        }
    }

    profile->max_state = i;
    profile->polling_ms = 50;
    profile->target = sky1_npu_devfreq_target;
    profile->get_dev_status = sky1_npu_devfreq_get_dev_status;
    profile->get_cur_freq = sky1_npu_devfreq_get_cur_freq;

    cix_aipu_priv->devfreq = devm_devfreq_add_device(dev, profile, DEVFREQ_GOV_USERSPACE, NULL);
    if (IS_ERR(cix_aipu_priv->devfreq)) {
        dev_err(dev, "Failed to add devfreq device");
        ret = PTR_ERR(cix_aipu_priv->devfreq);
        cix_aipu_priv->devfreq = NULL;
        goto remove_table;
    }

    ret = devm_devfreq_register_opp_notifier(dev, cix_aipu_priv->devfreq);
    if (ret < 0) {
        dev_err(dev, "Failed to register opp notifier");
        goto remove_device;
    }

    return ret;

remove_device:
    devm_devfreq_remove_device(dev, cix_aipu_priv->devfreq);
    cix_aipu_priv->devfreq = NULL;
remove_table:
    dev_pm_opp_remove_table(dev);
    profile->max_state = 0;
free_table:
    kfree(profile->freq_table);
    profile->freq_table = NULL;
unlink_opp:
    device_link_del(cix_aipu_priv->opp_dl);
    cix_aipu_priv->opp_dl = NULL;
detach_opp:
    dev_pm_domain_detach(cix_aipu_priv->opp_pmdomain, true);
    cix_aipu_priv->opp_pmdomain = NULL;

    return ret;
}

static int sky1_npu_devfreq_remove(struct device *dev, struct cix_aipu_priv *cix_aipu_priv)
{
    int i = 0;
    struct devfreq_dev_profile *profile;

    if (!cix_aipu_priv->opp_pmdomain)
        return 0;

    profile = &(cix_aipu_priv->devfreq_profile);

    if (cix_aipu_priv->devfreq) {
        devm_devfreq_unregister_opp_notifier(dev, cix_aipu_priv->devfreq);
        devm_devfreq_remove_device(dev, cix_aipu_priv->devfreq);
        cix_aipu_priv->devfreq = NULL;
    }

    for (i = 0; profile->freq_table && i < profile->max_state; i++) {
        if (profile->freq_table[i]) {
            dev_pm_opp_remove(dev, profile->freq_table[i]);
        }
    }

    dev_pm_opp_remove_table(dev);
    cix_aipu_priv->devfreq_profile.max_state = 0;
    kfree(cix_aipu_priv->devfreq_profile.freq_table);
    cix_aipu_priv->devfreq_profile.freq_table = NULL;

    if (cix_aipu_priv->opp_dl)
        device_link_del(cix_aipu_priv->opp_dl);
    cix_aipu_priv->opp_dl = NULL;
    dev_pm_domain_detach(cix_aipu_priv->opp_pmdomain, true);
    cix_aipu_priv->opp_pmdomain = NULL;

    remove_debugfs_dir("genpd:3:14260000.aipu");

    return 0;
}
#endif /* CONFIG_ENABLE_DEVFREQ */

/* balanced on failure, unlike a bare pm_runtime_get_sync() */
int sky1_npu_pm_runtime_get_sync(struct device *dev)
{
#ifdef CONFIG_PM
	int ret = 0;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		dev_info(dev, "PM runtime get sync failed! ret = %d", ret);

	return ret;
#else /* !CONFIG_PM  */
	return 0;
#endif /* CONFIG_PM */
}

/* pm_runtime_put() returns void since Linux 7.0 */
int sky1_npu_pm_runtime_put(struct device *dev)
{
	pm_runtime_put(dev);
	return 0;
}

/* -------------------------------------------------------------- DT */

static void sky1_npu_detach_pd(struct device *dev, struct aipu_soc *soc)
{
	int i = 0;

	dev_dbg(dev, "%s\n", __func__);

	for (i = 0; i < CIX_NPU_PD_MAX_NUM; i++) {
		if (cix_aipu_priv->pd_link[i]) {
			device_link_del(cix_aipu_priv->pd_link[i]);
			cix_aipu_priv->pd_link[i] = NULL;
		}
		if (!IS_ERR_OR_NULL(cix_aipu_priv->pd_core[i]))
			dev_pm_domain_detach(cix_aipu_priv->pd_core[i], true);
		cix_aipu_priv->pd_core[i] = NULL;
	}
	cix_aipu_priv->pd_cnt = 0;
}

static int sky1_npu_attach_pd(struct device *dev, struct aipu_soc *soc)
{
	int i = 0;
	struct device *pd = NULL;
	struct device_link *link = NULL;

	for (i = 0; i < CIX_NPU_PD_NUM; i++) {
		dev_dbg(dev, "%s\n", cix_npu_pd_names[i]);

		pd = dev_pm_domain_attach_by_name(dev, cix_npu_pd_names[i]);
		if (IS_ERR_OR_NULL(pd)) {
			/* NULL: no "power-domains"/"power-domain-names" in the DT node */
			dev_err(dev, "failed to get pd %s (%ld)\n", cix_npu_pd_names[i],
				pd ? PTR_ERR(pd) : -ENODEV);
			sky1_npu_detach_pd(dev, soc);
			return pd ? PTR_ERR(pd) : -ENODEV;
		}
		cix_aipu_priv->pd_core[i] = pd;
		cix_aipu_priv->pd_cnt = i + 1;

		link = device_link_add(dev, pd,
				DL_FLAG_STATELESS |
				DL_FLAG_PM_RUNTIME |
				DL_FLAG_RPM_ACTIVE);
		if (!link) {
			dev_err(dev, "Failed to add device_link to npu pd.\n");
			sky1_npu_detach_pd(dev, soc);
			return -EINVAL;
		}
		/* stateless links must be deleted explicitly (they were leaked) */
		cix_aipu_priv->pd_link[i] = link;
	}

	return 0;
}

/* ------------------------------------------------------------ ACPI */

static void sky1_npu_acpi_put_cores(struct device *dev)
{
	int i = 0;

	for (i = 0; i < CIX_NPU_PD_MAX_NUM; i++) {
		struct device *core = cix_aipu_priv->pd_core[i];

		if (!core)
			continue;
		dev_pm_domain_detach(core, true);
		pm_runtime_disable(core);
		/* reference taken by bus_find_device_by_fwnode() */
		put_device(core);
		cix_aipu_priv->pd_core[i] = NULL;
	}
	cix_aipu_priv->pd_cnt = 0;
}

static int sky1_npu_acpi_get_cores(struct device *dev)
{
#ifdef CONFIG_ACPI
	struct fwnode_handle *child = NULL;
	struct acpi_device *adev = NULL;
	struct device *core = NULL;
	int i = 0;
	int ret = 0;

	dev->power.ignore_children = true;

	fwnode_for_each_child_node(dev->fwnode, child) {
		if (is_acpi_data_node(child))
			continue;

		adev = to_acpi_device_node(child);
		if (!adev || strncmp(acpi_device_bid(adev), NPU_CORE_ACPI_NAME_PREFIX,
				     ACPI_NAMESEG_SIZE - 1))
			continue;

		if (i == CIX_NPU_PD_NUM) {
			/* leaving fwnode_for_each_child_node() early: drop its reference */
			fwnode_handle_put(child);
			break;
		}

		core = bus_find_device_by_fwnode(&platform_bus_type, child);
		if (!core) {
			/* used to be dereferenced as is: NULL pointer oops in probe */
			dev_warn(dev, "ACPI NPU core %s has no platform device, skipped\n",
				 acpi_device_bid(adev));
			continue;
		}

		pm_runtime_enable(core);
		ret = dev_pm_domain_attach(core, SKY1_PD_ATTACH_FLAGS);
		if (ret)
			dev_warn(dev, "ACPI NPU core %s: PM domain attach failed (%d)\n",
				 acpi_device_bid(adev), ret);
		dev_pm_set_driver_flags(core, DPM_FLAG_NO_DIRECT_COMPLETE);
		if (ACPI_COMPANION(core))
			ACPI_COMPANION(core)->power.flags.ignore_parent = true;

		cix_aipu_priv->pd_core[i] = core;
		cix_aipu_priv->pd_cnt = ++i;
	}

	if (!i) {
		dev_err(dev, "no ACPI NPU core (%s*) found\n", NPU_CORE_ACPI_NAME_PREFIX);
		return -ENODEV;
	}

	if (i < CIX_NPU_PD_NUM) {
		dev_warn(dev, "only %d of %d ACPI NPU cores found\n", i, CIX_NPU_PD_NUM);
		CIX_NPU_PD_NUM = i;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

/* resume_and_get every ACPI core; on failure the ones already taken are released */
static int sky1_npu_acpi_cores_resume(struct device *dev)
{
	int i = 0;
	int ret = 0;

	for (i = 0; i < cix_aipu_priv->pd_cnt; i++) {
		ret = pm_runtime_resume_and_get(cix_aipu_priv->pd_core[i]);
		if (ret < 0) {
			dev_err(cix_aipu_priv->pd_core[i],
				"NPU core PM runtime resume failed! ret=%d", ret);
			while (i-- > 0)
				pm_runtime_put(cix_aipu_priv->pd_core[i]);
			return ret;
		}
	}

	return 0;
}

static void sky1_npu_acpi_cores_put(struct device *dev, bool sync)
{
	int i = 0;

	for (i = 0; i < cix_aipu_priv->pd_cnt; i++) {
		if (sync)
			pm_runtime_put_sync(cix_aipu_priv->pd_core[i]);
		else
			pm_runtime_put(cix_aipu_priv->pd_core[i]);
	}
}

static void sky1_npu_release_pd(struct device *dev)
{
	if (has_acpi_companion(dev))
		sky1_npu_acpi_put_cores(dev);
	else
		sky1_npu_detach_pd(dev, &sky1);
}

static struct aipu_soc_operations sky1_ops = {
	.start_bw_profiling = NULL,
	.stop_bw_profiling = NULL,
	.read_profiling_reg = NULL,
	.enable_clk = NULL,
	.disable_clk = NULL,
	.is_clk_enabled = NULL,
	.is_aipu_irq = NULL,
};

static int sky1_npu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cix_aipu_priv *priv = NULL;
	bool acpi = has_acpi_companion(dev);
	u32 mask = 3;
	int ret = 0;

	/* a missing property keeps the default mask */
	device_property_read_u32(dev, "core_mask", &mask);
	if (mask == 0x0 || mask == 0x2) {
		/*
		 * Was "return 0": the device then looked bound without any
		 * private data and sky1_npu_remove() oopsed on unbind/rmmod.
		 */
		dev_info(dev, "NPU disabled by core_mask 0x%x\n", mask);
		return -ENODEV;
	}
	/* reset on every probe: the value survived unbind/bind cycles */
	CIX_NPU_PD_NUM = (mask == 0x1) ? 1 : CIX_NPU_PD_MAX_NUM;

	dev_info(dev, "%s: NPU core num is %d\n", __func__, CIX_NPU_PD_NUM);

	priv = sky1_priv_init(dev);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
	cix_aipu_priv = priv;

	sky1.priv = cix_aipu_priv;
	dev_set_drvdata(dev, cix_aipu_priv);

	if (acpi)
		ret = sky1_npu_acpi_get_cores(dev);
	else
		ret = sky1_npu_attach_pd(dev, &sky1);
	if (ret) {
		dev_err(dev, "aipu attach pd failed, ret: %d\n", ret);
		goto err_pd;
	}

#ifdef CONFIG_ENABLE_DEVFREQ
	ret = sky1_npu_devfreq_init(dev, cix_aipu_priv);
	if (ret) {
		dev_err(dev, "aipu devfreq init failed, ret: %d\n", ret);
		goto err_pd;
	}
#endif

	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	if (acpi) {
		ret = sky1_npu_acpi_cores_resume(dev);
		if (ret < 0)
			goto err_rpm;
	}

	ret = armchina_aipu_probe(pdev, &sky1, &sky1_ops);
	if (ret) {
		dev_err(dev, "aipu real probe failed, ret: %d\n", ret);
		goto err_cores;
	}

	dev_info(dev, "%s: armchina_aipu_probe done\n", __func__);

	/* drop the probe reference: the NPU suspends until /dev/aipu is opened */
	pm_runtime_put(dev);

	return 0;

err_cores:
	if (acpi)
		sky1_npu_acpi_cores_put(dev, true);
err_rpm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_put_noidle(dev);
#ifdef CONFIG_ENABLE_DEVFREQ
	sky1_npu_devfreq_remove(dev, cix_aipu_priv);
#endif
err_pd:
	sky1_npu_release_pd(dev);
	sky1.priv = NULL;
	cix_aipu_priv = NULL;
	return ret;
}

static void sky1_npu_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	bool acpi = has_acpi_companion(dev);
	int ret = 0;

	dev_dbg(dev, "%s\n", __func__);

	if (!cix_aipu_priv)
		return;

	/*
	 * Power the NPU (and, on ACPI, its cores) for the teardown, so that the
	 * runtime PM usage counts of the NPU and of the CREx devices end up
	 * balanced: the old code left the cores enabled with a usage count,
	 * which broke the next insmod ("Unbalanced pm_runtime_enable").
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		dev_warn(dev, "%s: runtime resume failed (%d)\n", __func__, ret);

#ifdef CONFIG_ENABLE_DEVFREQ
	sky1_npu_devfreq_remove(dev, cix_aipu_priv);
#endif

	armchina_aipu_remove(pdev);

	pm_runtime_disable(dev);
	if (ret >= 0) {
		pm_runtime_put_noidle(dev);
		/* the NPU is active: its cores hold one runtime PM reference */
		if (acpi)
			sky1_npu_acpi_cores_put(dev, true);
	}
	pm_runtime_set_suspended(dev);

	sky1_npu_release_pd(dev);
	sky1.priv = NULL;
	cix_aipu_priv = NULL;
}

#ifdef CONFIG_PM
static int sky1_npu_runtime_suspend(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	pm_message_t state;
	state.event = 0;

	ret = armchina_aipu_suspend(pdev, state);
	if (ret) {
		dev_err(dev, "aipu is busy, %s return %d\n", __func__, ret);
		return ret;
	}

	if (has_acpi_companion(dev) && cix_aipu_priv)
		sky1_npu_acpi_cores_put(dev, false);

	return 0;
}

static int sky1_npu_runtime_resume(struct device *dev)
{
	int ret;
	struct platform_device *pdev = to_platform_device(dev);
	bool acpi = has_acpi_companion(dev) && cix_aipu_priv;

	if (acpi) {
		/*
		 * pm_runtime_get_sync() keeps the usage count raised on failure:
		 * the old loop leaked it, and the cores already resumed.
		 */
		ret = sky1_npu_acpi_cores_resume(dev);
		if (ret < 0)
			return ret;
	}

	ret = armchina_aipu_resume(pdev);
	if (ret && acpi)
		sky1_npu_acpi_cores_put(dev, false);

	return ret;
}

static void npu_complete(struct device *dev) {
	if (has_acpi_companion(dev))
		pm_request_resume(dev);
}

static const struct dev_pm_ops cix_sky1_npu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(sky1_npu_runtime_suspend, sky1_npu_runtime_resume, NULL)
	.complete = npu_complete,
};
#endif /* CONFIG_PM */

#ifdef CONFIG_OF
static const struct of_device_id aipu_of_match[] = {
	{
		.compatible = "armchina,zhouyi-v1",
	},
	{
		.compatible = "armchina,zhouyi-v2",
	},
	{
		.compatible = "armchina,zhouyi-v3",
	},
	{
		.compatible = "armchina,zhouyi",
	},
	{ }
};

MODULE_DEVICE_TABLE(of, aipu_of_match);
#endif

/* the Orange Pi 6 Plus UEFI firmware describes the NPU in ACPI (CIXH4000) */
static const struct acpi_device_id aipu_acpi_match[] __maybe_unused = {
	{ .id = "CIXH4000", .driver_data = 0 },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(acpi, aipu_acpi_match);

static struct platform_driver aipu_platform_driver = {
	.probe = sky1_npu_probe,
	.remove = sky1_npu_remove,
	.driver = {
		.name = "armchina",
#ifdef CONFIG_PM
		.pm = &cix_sky1_npu_pm_ops,
#endif /* CONFIG_PM */
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(aipu_of_match),
#endif
		/* was inside #ifdef CONFIG_OF: ACPI matching needs no OF */
		.acpi_match_table = ACPI_PTR(aipu_acpi_match),
	},
};

module_platform_driver(aipu_platform_driver);
MODULE_DESCRIPTION("ArmChina Zhouyi V3 NPU driver for the CIX SKY1 / CD8180 SoC");
MODULE_LICENSE("GPL v2");
