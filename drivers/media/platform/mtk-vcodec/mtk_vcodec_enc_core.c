// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "mtk_vcodec_drv.h"
#include "mtk_vcodec_enc.h"
#include "mtk_vcodec_enc_core.h"

static const struct of_device_id mtk_venc_core_ids[] = {
	{
		.compatible = "mediatek,mtk-venc-core0",
		.data = (void *)MTK_VENC_CORE0,
	},
	{
		.compatible = "mediatek,mtk-venc-core1",
		.data = (void *)MTK_VENC_CORE1,
	},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_venc_core_ids);

static void clean_irq_status(unsigned int irq_status, void __iomem *addr)
{
	if (irq_status & MTK_VENC_IRQ_STATUS_PAUSE)
		writel(MTK_VENC_IRQ_STATUS_PAUSE, addr);

	if (irq_status & MTK_VENC_IRQ_STATUS_SWITCH)
		writel(MTK_VENC_IRQ_STATUS_SWITCH, addr);

	if (irq_status & MTK_VENC_IRQ_STATUS_DRAM)
		writel(MTK_VENC_IRQ_STATUS_DRAM, addr);

	if (irq_status & MTK_VENC_IRQ_STATUS_SPS)
		writel(MTK_VENC_IRQ_STATUS_SPS, addr);

	if (irq_status & MTK_VENC_IRQ_STATUS_PPS)
		writel(MTK_VENC_IRQ_STATUS_PPS, addr);

	if (irq_status & MTK_VENC_IRQ_STATUS_FRM)
		writel(MTK_VENC_IRQ_STATUS_FRM, addr);
}

static irqreturn_t mtk_enc_core_irq_handler(int irq, void *priv)
{
	struct mtk_venc_core_dev *core = priv;
	struct mtk_vcodec_ctx *ctx;
	unsigned long flags;
	void __iomem *addr;

	spin_lock_irqsave(&core->main_dev->irqlock, flags);
	ctx = core->curr_ctx;
	spin_unlock_irqrestore(&core->main_dev->irqlock, flags);
	if (!ctx)
		return IRQ_HANDLED;

	mtk_v4l2_debug(1, "id=%d core :%d", ctx->id, core->core_id);

	addr = core->reg_base + MTK_VENC_IRQ_ACK_OFFSET;
	ctx->irq_status = readl(core->reg_base + MTK_VENC_IRQ_STATUS_OFFSET);
	clean_irq_status(ctx->irq_status, addr);

	wake_up_ctx(ctx, MTK_INST_IRQ_RECEIVED, 0);
	return IRQ_HANDLED;
}

static int mtk_venc_core_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_venc_core_dev *core;
	struct mtk_vcodec_dev *main_dev;
	int ret;

	if (!dev->parent) {
		dev_err(dev, "No parent for venc core device\n");
		return -ENODEV;
	}

	main_dev = dev_get_drvdata(dev->parent);
	if (!main_dev) {
		dev_err(dev, "Failed to get parent driver data");
		return -EINVAL;
	}

	core = devm_kzalloc(&pdev->dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->plat_dev = pdev;

	core->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(core->reg_base)) {
		dev_err(&pdev->dev, "Failed to get reg base");
		ret = PTR_ERR(core->reg_base);
		goto err;
	}

	core->enc_irq = platform_get_irq(pdev, 0);
	if (core->enc_irq < 0) {
		dev_err(&pdev->dev, "Failed to get irq resource");
		ret = core->enc_irq;
		goto err;
	}

	ret = devm_request_irq(&pdev->dev, core->enc_irq,
			       mtk_enc_core_irq_handler, 0,
			       pdev->name, core);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to install core->enc_irq %d (%d)",
			core->enc_irq, ret);
		ret = -EINVAL;
		goto err;
	}

	core->core_id =
		(enum mtk_venc_core_id)of_device_get_match_data(&pdev->dev);
	if (core->core_id < 0 || core->core_id >= MTK_VENC_CORE_MAX) {
		ret = -EINVAL;
		goto err;
	}

	main_dev->enc_core_dev[core->core_id] = core;
	core->main_dev = main_dev;

	platform_set_drvdata(pdev, core);

	dev_info(dev, "Venc core :%d probe done\n", core->core_id);

	return 0;

err:
	return ret;
}

static struct platform_driver mtk_venc_core_driver = {
	.probe  = mtk_venc_core_probe,
	.driver = {
		.name	 = "mtk-venc-core",
		.of_match_table = mtk_venc_core_ids,
	},
};
module_platform_driver(mtk_venc_core_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Mediatek video encoder core driver");
