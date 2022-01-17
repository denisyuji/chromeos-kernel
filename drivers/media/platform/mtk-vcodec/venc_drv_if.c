// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Daniel Hsiao <daniel.hsiao@mediatek.com>
 *	Jungchang Tsao <jungchang.tsao@mediatek.com>
 *	Tiffany Lin <tiffany.lin@mediatek.com>
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "venc_drv_base.h"
#include "venc_drv_if.h"

#include "mtk_vcodec_enc.h"
#include "mtk_vcodec_enc_core.h"
#include "mtk_vcodec_enc_pm.h"

int venc_if_init(struct mtk_vcodec_ctx *ctx, unsigned int fourcc)
{
	int ret = 0;

	switch (fourcc) {
	case V4L2_PIX_FMT_VP8:
		ctx->enc_if = &venc_vp8_if;
		break;
	case V4L2_PIX_FMT_H264:
		ctx->enc_if = &venc_h264_if;
		break;
	default:
		return -EINVAL;
	}

	mtk_venc_lock(ctx, ctx->core_id);
	ret = ctx->enc_if->init(ctx);
	mtk_venc_unlock(ctx, ctx->core_id);

	return ret;
}

int venc_if_set_param(struct mtk_vcodec_ctx *ctx,
		enum venc_set_param_type type, struct venc_enc_param *in)
{
	int ret = 0;

	mtk_venc_lock(ctx, ctx->core_id);
	ret = ctx->enc_if->set_param(ctx->drv_handle, type, in);
	mtk_venc_unlock(ctx, ctx->core_id);

	return ret;
}

int venc_if_encode(struct mtk_vcodec_ctx *ctx,
		   enum venc_start_opt opt, struct venc_frm_buf *frm_buf,
		   struct mtk_vcodec_mem *bs_buf,
		   struct venc_done_result *result)
{
	int ret = 0;

	venc_encode_prepare(ctx, opt);

	ret = ctx->enc_if->encode(ctx->drv_handle, opt, frm_buf,
				  bs_buf, result);

	venc_encode_unprepare(ctx, opt);

	return ret;
}

int venc_if_deinit(struct mtk_vcodec_ctx *ctx)
{
	int ret = 0;

	if (!ctx->drv_handle)
		return 0;

	mtk_venc_lock(ctx, ctx->core_id);
	ret = ctx->enc_if->deinit(ctx->drv_handle);
	mtk_venc_unlock(ctx, ctx->core_id);

	ctx->drv_handle = NULL;

	return ret;
}

void venc_encode_prepare(struct mtk_vcodec_ctx *ctx,
			 enum venc_start_opt opt)
{
	unsigned long flags;
	struct mtk_venc_core_dev *core;

	if (ctx->dev->venc_pdata->core_mode == VENC_DUAL_CORE_MODE) {
		if (ctx->enc_idx & 0x01)
			ctx->core_id = MTK_VENC_CORE1;
		else
			ctx->core_id = MTK_VENC_CORE0;
	} else {
		ctx->core_id = MTK_VENC_CORE0;
	}
	mtk_venc_lock(ctx, ctx->core_id);

	spin_lock_irqsave(&ctx->dev->irqlock, flags);

	if (ctx->dev->venc_pdata->core_mode == VENC_DUAL_CORE_MODE) {
		core = ctx->dev->enc_core_dev[ctx->core_id];

		core->curr_ctx = ctx;
	} else {
		ctx->dev->curr_ctx = ctx;
	}

	spin_unlock_irqrestore(&ctx->dev->irqlock, flags);

	mtk_vcodec_enc_clock_on(ctx->dev, ctx->core_id);
}

void venc_encode_unprepare(struct mtk_vcodec_ctx *ctx,
			   enum venc_start_opt opt)
{
	unsigned long flags;

	if (ctx->dev->venc_pdata->core_mode == VENC_SINGLE_CORE_MODE ||
	    opt == VENC_START_OPT_ENCODE_SEQUENCE_HEADER) {
		mtk_vcodec_enc_clock_off(ctx->dev, ctx->core_id);
		spin_lock_irqsave(&ctx->dev->irqlock, flags);
		ctx->dev->curr_ctx = NULL;
		spin_unlock_irqrestore(&ctx->dev->irqlock, flags);
		mtk_venc_unlock(ctx, ctx->core_id);
	}
}
