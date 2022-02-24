// SPDX-License-Identifier: GPL-2.0
/*
* Copyright (c) 2016 MediaTek Inc.
* Author: Tiffany Lin <tiffany.lin@mediatek.com>
*/

#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>

#include "mtk_vcodec_enc_core.h"
#include "mtk_vcodec_enc_pm.h"
#include "mtk_vcodec_util.h"

int mtk_vcodec_init_enc_clk(struct platform_device *pdev,
			    struct mtk_vcodec_pm *pm)
{
	struct mtk_vcodec_clk *enc_clk;
	struct mtk_vcodec_clk_info *clk_info;
	int ret, i;

	memset(pm, 0, sizeof(struct mtk_vcodec_pm));
	pm->dev = &pdev->dev;
	enc_clk = &pm->venc_clk;

	enc_clk->clk_num = of_property_count_strings(pdev->dev.of_node,
		"clock-names");
	if (enc_clk->clk_num > 0) {
		enc_clk->clk_info = devm_kcalloc(&pdev->dev,
			enc_clk->clk_num, sizeof(*clk_info),
			GFP_KERNEL);
		if (!enc_clk->clk_info)
			return -ENOMEM;
	} else {
		mtk_v4l2_err("Failed to get venc clock count");
		return -EINVAL;
	}

	for (i = 0; i < enc_clk->clk_num; i++) {
		clk_info = &enc_clk->clk_info[i];
		ret = of_property_read_string_index(pdev->dev.of_node,
			"clock-names", i, &clk_info->clk_name);
		if (ret) {
			mtk_v4l2_err("venc failed to get clk name %d", i);
			return ret;
		}
		clk_info->vcodec_clk = devm_clk_get(&pdev->dev,
			clk_info->clk_name);
		if (IS_ERR(clk_info->vcodec_clk)) {
			mtk_v4l2_err("venc devm_clk_get (%d)%s fail", i,
				clk_info->clk_name);
			return PTR_ERR(clk_info->vcodec_clk);
		}
	}

	return 0;
}
EXPORT_SYMBOL(mtk_vcodec_init_enc_clk);

void mtk_vcodec_enc_clock_on(struct mtk_vcodec_dev *dev, int core_id)
{
	struct mtk_venc_core_dev *core;
	struct mtk_vcodec_pm *enc_pm;
	struct mtk_vcodec_clk *enc_clk;
	struct clk              *clk;
	int ret, i = 0;

	if (dev->venc_pdata->core_mode == VENC_DUAL_CORE_MODE) {
		core = (struct mtk_venc_core_dev *)dev->enc_core_dev[core_id];
		enc_pm = &core->pm;
		enc_clk = &enc_pm->venc_clk;

		for (i = 0; i < enc_clk->clk_num; i++) {
			clk = enc_clk->clk_info[i].vcodec_clk;
			ret = clk_enable(clk);
			if (ret) {
				mtk_v4l2_err("clk_enable %d %s fail %d", i,
					     enc_clk->clk_info[i].clk_name,
					     ret);
				goto core_clk_err;
			}
		}
	} else {
		enc_pm = &dev->pm;
		enc_clk = &enc_pm->venc_clk;

		for (i = 0; i < enc_clk->clk_num; i++) {
			clk = enc_clk->clk_info[i].vcodec_clk;
			ret = clk_prepare_enable(clk);
			if (ret) {
				mtk_v4l2_err("clk_prepare %d %s fail %d",
					     i, enc_clk->clk_info[i].clk_name,
					     ret);
				goto clkerr;
			}
		}
	}

	return;

core_clk_err:
	for (i -= 1; i >= 0; i--)
		clk_disable(enc_clk->clk_info[i].vcodec_clk);

	return;

clkerr:
	for (i -= 1; i >= 0; i--)
		clk_disable_unprepare(enc_clk->clk_info[i].vcodec_clk);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_enc_clock_on);

void mtk_vcodec_enc_clock_off(struct mtk_vcodec_dev *dev, int core_id)
{
	struct mtk_venc_core_dev *core;
	struct mtk_vcodec_pm *enc_pm;
	struct mtk_vcodec_clk *enc_clk;
	int i = 0;

	if (dev->venc_pdata->core_mode == VENC_DUAL_CORE_MODE) {
		core = (struct mtk_venc_core_dev *)dev->enc_core_dev[core_id];
		enc_pm = &core->pm;
		enc_clk = &enc_pm->venc_clk;

		for (i = enc_clk->clk_num - 1; i >= 0; i--)
			clk_disable(enc_clk->clk_info[i].vcodec_clk);
	} else {
		enc_pm = &dev->pm;
		enc_clk = &enc_pm->venc_clk;

		for (i = enc_clk->clk_num - 1; i >= 0; i--)
			clk_disable_unprepare(enc_clk->clk_info[i].vcodec_clk);
	}
}
EXPORT_SYMBOL_GPL(mtk_vcodec_enc_clock_off);

int mtk_venc_core_pw_on(struct mtk_vcodec_dev *dev)
{
	int i, ret, j = 0;
	struct mtk_venc_core_dev *core;
	struct mtk_vcodec_clk *clk;

	/* power on all available venc cores */
	for (i = 0; i < MTK_VENC_CORE_MAX; i++) {
		core = (struct mtk_venc_core_dev *)dev->enc_core_dev[i];
		if (!core)
			return 0;

		ret = pm_runtime_resume_and_get(&core->plat_dev->dev);
		if (ret < 0) {
			mtk_v4l2_err("power on core[%d] fail %d", i, ret);
			goto pw_on_fail;
		}

		clk = &core->pm.venc_clk;
		for (j = 0; j < clk->clk_num; j++) {
			ret = clk_prepare(clk->clk_info[j].vcodec_clk);
			if (ret) {
				mtk_v4l2_err("prepare clk [%s] fail %d",
					     clk->clk_info[j].clk_name,
					     ret);
				goto pw_on_fail;
			}
		}
	}
	return 0;

pw_on_fail:
	for (i -= 1; i >= 0; i--) {
		core = (struct mtk_venc_core_dev *)dev->enc_core_dev[i];

		clk = &core->pm.venc_clk;
		for (j -= 1; j >= 0; j--)
			clk_unprepare(clk->clk_info[j].vcodec_clk);

		pm_runtime_put_sync(&core->plat_dev->dev);
	}
	return ret;
}

int mtk_venc_core_pw_off(struct mtk_vcodec_dev *dev)
{
	int i, ret, j;
	struct mtk_venc_core_dev *core;
	struct mtk_vcodec_clk *clk;

	/* power off all available venc cores */
	for (i = 0; i < MTK_VENC_CORE_MAX; i++) {
		core = (struct mtk_venc_core_dev *)dev->enc_core_dev[i];
		if (!core)
			return 0;

		clk = &core->pm.venc_clk;
		for (j = clk->clk_num - 1; j >= 0; j--)
			clk_unprepare(clk->clk_info[j].vcodec_clk);

		ret = pm_runtime_put_sync(&core->plat_dev->dev);
		if (ret < 0)
			mtk_v4l2_err("power off core[%d] fail %d", i, ret);
	}
	return ret;
}

int mtk_vcodec_enc_pw_on(struct mtk_vcodec_dev *dev)
{
	int ret;

	if (dev->venc_pdata->core_mode == VENC_DUAL_CORE_MODE) {
		ret = mtk_venc_core_pw_on(dev);
		if (ret < 0) {
			mtk_v4l2_err("venc core power on fail: %d", ret);
			return ret;
		}
	} else {
		ret = pm_runtime_resume_and_get(&dev->plat_dev->dev);
		if (ret < 0) {
			mtk_v4l2_err("pm_runtime_resume_and_get fail %d", ret);
			return ret;
		}
	}
	return 0;
}

int mtk_vcodec_enc_pw_off(struct mtk_vcodec_dev *dev)
{
	int ret;

	if (dev->venc_pdata->core_mode == VENC_DUAL_CORE_MODE) {
		ret = mtk_venc_core_pw_off(dev);
		if (ret < 0)
			mtk_v4l2_err("venc core power off fail: %d", ret);

	} else {
		ret = pm_runtime_put_sync(&dev->plat_dev->dev);
		if (ret < 0)
			mtk_v4l2_err("pm_runtime_put_sync fail %d", ret);
	}
	return ret;
}
