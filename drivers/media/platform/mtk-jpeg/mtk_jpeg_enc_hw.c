// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019 MediaTek Inc.
 * Author: Xia Jiang <xia.jiang@mediatek.com>
 *
 */
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <media/media-device.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>

#include "mtk_jpeg_core.h"
#include "mtk_jpeg_enc_hw.h"

static const struct mtk_jpeg_enc_qlt mtk_jpeg_enc_quality[] = {
	{.quality_param = 34, .hardware_value = JPEG_ENC_QUALITY_Q34},
	{.quality_param = 39, .hardware_value = JPEG_ENC_QUALITY_Q39},
	{.quality_param = 48, .hardware_value = JPEG_ENC_QUALITY_Q48},
	{.quality_param = 60, .hardware_value = JPEG_ENC_QUALITY_Q60},
	{.quality_param = 64, .hardware_value = JPEG_ENC_QUALITY_Q64},
	{.quality_param = 68, .hardware_value = JPEG_ENC_QUALITY_Q68},
	{.quality_param = 74, .hardware_value = JPEG_ENC_QUALITY_Q74},
	{.quality_param = 80, .hardware_value = JPEG_ENC_QUALITY_Q80},
	{.quality_param = 82, .hardware_value = JPEG_ENC_QUALITY_Q82},
	{.quality_param = 84, .hardware_value = JPEG_ENC_QUALITY_Q84},
	{.quality_param = 87, .hardware_value = JPEG_ENC_QUALITY_Q87},
	{.quality_param = 90, .hardware_value = JPEG_ENC_QUALITY_Q90},
	{.quality_param = 92, .hardware_value = JPEG_ENC_QUALITY_Q92},
	{.quality_param = 95, .hardware_value = JPEG_ENC_QUALITY_Q95},
	{.quality_param = 97, .hardware_value = JPEG_ENC_QUALITY_Q97},
};

#if defined(CONFIG_OF)
static const struct of_device_id mtk_jpegenc_drv_ids[] = {
	{
		.compatible = "mediatek,mt8195-jpgenc0",
		.data = (void *)MTK_JPEGENC_HW0,
	},
	{
		.compatible = "mediatek,mt8195-jpgenc1",
		.data = (void *)MTK_JPEGENC_HW1,
	},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_jpegenc_drv_ids);
#endif

void mtk_jpeg_enc_reset(void __iomem *base)
{
	writel(0, base + JPEG_ENC_RSTB);
	writel(JPEG_ENC_RESET_BIT, base + JPEG_ENC_RSTB);
	writel(0, base + JPEG_ENC_CODEC_SEL);
}

u32 mtk_jpeg_enc_get_file_size(void __iomem *base)
{
	return readl(base + JPEG_ENC_DMA_ADDR0) -
	       readl(base + JPEG_ENC_DST_ADDR0);
}

void mtk_jpeg_enc_start(void __iomem *base)
{
	u32 value;

	value = readl(base + JPEG_ENC_CTRL);
	value |= JPEG_ENC_CTRL_INT_EN_BIT | JPEG_ENC_CTRL_ENABLE_BIT;
	writel(value, base + JPEG_ENC_CTRL);
}

void mtk_jpeg_set_enc_src(struct mtk_jpeg_ctx *ctx,  void __iomem *base,
			  struct vb2_buffer *src_buf)
{
	int i;
	dma_addr_t dma_addr;

	for (i = 0; i < src_buf->num_planes; i++) {
		dma_addr = vb2_dma_contig_plane_dma_addr(src_buf, i) +
			   src_buf->planes[i].data_offset;
		if (!i)
			writel(dma_addr, base + JPEG_ENC_SRC_LUMA_ADDR);
		else
			writel(dma_addr, base + JPEG_ENC_SRC_CHROMA_ADDR);
	}
}

void mtk_jpeg_set_enc_dst(struct mtk_jpeg_ctx *ctx, void __iomem *base,
			  struct vb2_buffer *dst_buf)
{
	dma_addr_t dma_addr;
	size_t size;
	u32 dma_addr_offset;
	u32 dma_addr_offsetmask;

	dma_addr = vb2_dma_contig_plane_dma_addr(dst_buf, 0);
	dma_addr_offset = ctx->enable_exif ? MTK_JPEG_MAX_EXIF_SIZE : 0;
	dma_addr_offsetmask = dma_addr & JPEG_ENC_DST_ADDR_OFFSET_MASK;
	size = vb2_plane_size(dst_buf, 0);

	writel(dma_addr_offset & ~0xf, base + JPEG_ENC_OFFSET_ADDR);
	writel(dma_addr_offsetmask & 0xf, base + JPEG_ENC_BYTE_OFFSET_MASK);
	writel(dma_addr & ~0xf, base + JPEG_ENC_DST_ADDR0);
	writel((dma_addr + size) & ~0xf, base + JPEG_ENC_STALL_ADDR0);
}

void mtk_jpeg_set_enc_params(struct mtk_jpeg_ctx *ctx,  void __iomem *base)
{
	u32 value;
	u32 width = ctx->out_q.enc_crop_rect.width;
	u32 height = ctx->out_q.enc_crop_rect.height;
	u32 enc_format = ctx->out_q.fmt->fourcc;
	u32 bytesperline = ctx->out_q.pix_mp.plane_fmt[0].bytesperline;
	u32 blk_num;
	u32 img_stride;
	u32 mem_stride;
	u32 i, enc_quality;

	value = width << 16 | height;
	writel(value, base + JPEG_ENC_IMG_SIZE);

	if (enc_format == V4L2_PIX_FMT_NV12M ||
	    enc_format == V4L2_PIX_FMT_NV21M)
	    /*
	     * Total 8 x 8 block number of luma and chroma.
	     * The number of blocks is counted from 0.
	     */
		blk_num = DIV_ROUND_UP(width, 16) *
			  DIV_ROUND_UP(height, 16) * 6 - 1;
	else
		blk_num = DIV_ROUND_UP(width, 16) *
			  DIV_ROUND_UP(height, 8) * 4 - 1;
	writel(blk_num, base + JPEG_ENC_BLK_NUM);

	if (enc_format == V4L2_PIX_FMT_NV12M ||
	    enc_format == V4L2_PIX_FMT_NV21M) {
		/* 4:2:0 */
		img_stride = round_up(width, 16);
		mem_stride = bytesperline;
	} else {
		/* 4:2:2 */
		img_stride = round_up(width * 2, 32);
		mem_stride = img_stride;
	}
	writel(img_stride, base + JPEG_ENC_IMG_STRIDE);
	writel(mem_stride, base + JPEG_ENC_STRIDE);

	enc_quality = mtk_jpeg_enc_quality[0].hardware_value;
	for (i = 0; i < ARRAY_SIZE(mtk_jpeg_enc_quality); i++) {
		if (ctx->enc_quality <= mtk_jpeg_enc_quality[i].quality_param) {
			enc_quality = mtk_jpeg_enc_quality[i].hardware_value;
			break;
		}
	}
	writel(enc_quality, base + JPEG_ENC_QUALITY);

	value = readl(base + JPEG_ENC_CTRL);
	value &= ~JPEG_ENC_CTRL_YUV_FORMAT_MASK;
	value |= (ctx->out_q.fmt->hw_format & 3) << 3;
	if (ctx->enable_exif)
		value |= JPEG_ENC_CTRL_FILE_FORMAT_BIT;
	else
		value &= ~JPEG_ENC_CTRL_FILE_FORMAT_BIT;
	if (ctx->restart_interval)
		value |= JPEG_ENC_CTRL_RESTART_EN_BIT;
	else
		value &= ~JPEG_ENC_CTRL_RESTART_EN_BIT;
	writel(value, base + JPEG_ENC_CTRL);

	writel(ctx->restart_interval, base + JPEG_ENC_RST_MCU_NUM);
}

static irqreturn_t mtk_jpegenc_hw_irq_handler(int irq, void *priv)
{
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	enum vb2_buffer_state buf_state;
	struct mtk_jpeg_ctx *ctx;
	u32 result_size;
	u32 irq_status;

	struct mtk_jpegenc_comp_dev *jpeg = priv;
	struct mtk_jpeg_dev *master_jpeg = jpeg->master_dev;

	irq_status = readl(jpeg->reg_base + JPEG_ENC_INT_STS) &
		JPEG_ENC_INT_STATUS_MASK_ALLIRQ;
	if (irq_status)
		writel(0, jpeg->reg_base + JPEG_ENC_INT_STS);
	if (!(irq_status & JPEG_ENC_INT_STATUS_DONE))
		return IRQ_NONE;

	ctx = v4l2_m2m_get_curr_priv(master_jpeg->m2m_dev);
	if (!ctx) {
		v4l2_err(&master_jpeg->v4l2_dev, "Context is NULL\n");
		return IRQ_HANDLED;
	}

	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	result_size = mtk_jpeg_enc_get_file_size(jpeg->reg_base);
	vb2_set_plane_payload(&dst_buf->vb2_buf, 0, result_size);
	buf_state = VB2_BUF_STATE_DONE;
	v4l2_m2m_buf_done(src_buf, buf_state);
	v4l2_m2m_buf_done(dst_buf, buf_state);
	v4l2_m2m_job_finish(master_jpeg->m2m_dev, ctx->fh.m2m_ctx);
	pm_runtime_put(ctx->jpeg->dev);

	return IRQ_HANDLED;
}

static int mtk_jpegenc_hw_init_irq(struct mtk_jpegenc_comp_dev *dev)
{
	struct platform_device *pdev = dev->plat_dev;
	int ret;

	dev->jpegenc_irq = platform_get_irq(pdev, 0);
	if (dev->jpegenc_irq < 0) {
		dev_err(&pdev->dev, "Failed to get irq resource");
		return dev->jpegenc_irq;
	}

	ret = devm_request_irq(&pdev->dev, dev->jpegenc_irq,
		mtk_jpegenc_hw_irq_handler, 0, pdev->name, dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to devm_request_irq %d (%d)",
			dev->jpegenc_irq, ret);
		return -ENOENT;
	}

	return 0;
}

int mtk_jpegenc_init_pm(struct mtk_jpegenc_comp_dev *mtkdev)
{
	struct mtk_jpegenc_clk_info *clk_info;
	struct mtk_jpegenc_clk *jpegenc_clk;
	struct platform_device *pdev;
	struct mtk_jpegenc_pm *pm;
	int i, ret;

	pdev = mtkdev->plat_dev;
	pm = &mtkdev->pm;
	pm->dev = &pdev->dev;
	pm->mtkdev = mtkdev;
	jpegenc_clk = &pm->venc_clk;
	jpegenc_clk->clk_num =
		of_property_count_strings(pdev->dev.of_node, "clock-names");
	if (!jpegenc_clk->clk_num) {
		dev_err(&pdev->dev, "Failed to get jpegenc clock count\n");
		return -EINVAL;
	}

	jpegenc_clk->clk_info = devm_kcalloc(&pdev->dev,
		jpegenc_clk->clk_num,
		sizeof(*clk_info),
		GFP_KERNEL);
	if (!jpegenc_clk->clk_info)
		return -ENOMEM;

	for (i = 0; i < jpegenc_clk->clk_num; i++) {
		clk_info = &jpegenc_clk->clk_info[i];
		ret = of_property_read_string_index(pdev->dev.of_node,
			"clock-names", i,
			&clk_info->clk_name);
		if (ret) {
			dev_err(&pdev->dev, "Failed to get jpegenc clock name\n");
			return ret;
		}

		clk_info->jpegenc_clk = devm_clk_get(&pdev->dev,
			clk_info->clk_name);
		if (IS_ERR(clk_info->jpegenc_clk)) {
			dev_err(&pdev->dev, "devm_clk_get (%d)%s fail",
				i, clk_info->clk_name);
			return PTR_ERR(clk_info->jpegenc_clk);
		}
	}

	pm_runtime_enable(&pdev->dev);

	return ret;
}

void mtk_jpegenc_release_pm(struct mtk_jpegenc_comp_dev *mtkdev)
{
	struct platform_device *pdev = mtkdev->plat_dev;

	pm_runtime_disable(&pdev->dev);
}

static int mtk_jpegenc_hw_probe(struct platform_device *pdev)
{
	struct mtk_jpeg_dev *master_dev;
	const struct of_device_id *of_id;
	struct mtk_jpegenc_comp_dev *dev;
	int ret, comp_idx;

	struct device *decs = &pdev->dev;

	if (!decs->parent)
		return -EPROBE_DEFER;

	master_dev = dev_get_drvdata(decs->parent);
	if (!master_dev)
		return -EPROBE_DEFER;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->plat_dev = pdev;

	ret = mtk_jpegenc_init_pm(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get jpeg enc clock source");
		return ret;
	}

	dev->reg_base =
		devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->reg_base)) {
		ret = PTR_ERR(dev->reg_base);
		goto err;
	}

	ret = mtk_jpegenc_hw_init_irq(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register JPEGENC irq handler.\n");
		goto err;
	}

	of_id = of_match_device(mtk_jpegenc_drv_ids, decs);
	if (!of_id) {
		dev_err(&pdev->dev, "Can't get vdec comp device id.\n");
		ret = -EINVAL;
		goto err;
	}

	comp_idx = (enum mtk_jpegenc_hw_id)of_id->data;
	if (comp_idx < MTK_JPEGENC_HW_MAX) {
		master_dev->enc_hw_dev[comp_idx] = dev;
		master_dev->reg_encbase[comp_idx] = dev->reg_base;
		dev->master_dev = master_dev;
	}

	platform_set_drvdata(pdev, dev);

	return 0;

err:
	mtk_jpegenc_release_pm(dev);
	return ret;
}


static int mtk_jpegenc_remove(struct platform_device *pdev)
{
	struct mtk_jpegenc_comp_dev *dev = platform_get_drvdata(pdev);

	mtk_jpegenc_release_pm(dev);

	return 0;
}

struct platform_driver mtk_jpegenc_hw_driver = {
	.probe = mtk_jpegenc_hw_probe,
	.remove = mtk_jpegenc_remove,
	.driver = {
		.name = "mtk-jpegenc-hw",
		.of_match_table = of_match_ptr(mtk_jpegenc_drv_ids),
	},
};
