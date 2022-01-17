/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef _MTK_VCODEC_ENC_CORE_H_
#define _MTK_VCODEC_ENC_CORE_H_

#include <linux/platform_device.h>
#include "mtk_vcodec_drv.h"

/*
 * struct mtk_venc_core_dev - driver data
 * @plat_dev: platform_device
 * @main_dev: main device
 * @pm: power management data
 * @curr_ctx: the context that is waiting for venc hardware
 * @reg_base: mapped address of venc registers
 * @irq_status: venc core irq status
 * @enc_irq: venc device irq
 * @core id: for venc core id: core#0, core#1...
 */
struct mtk_venc_core_dev {
	struct platform_device *plat_dev;
	struct mtk_vcodec_dev *main_dev;

	struct mtk_vcodec_pm pm;
	struct mtk_vcodec_ctx *curr_ctx;

	void __iomem *reg_base;
	unsigned int irq_status;
	int enc_irq;
	int core_id;
};

#endif /* _MTK_VCODEC_ENC_CORE_H_ */
