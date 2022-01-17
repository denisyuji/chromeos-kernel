/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Author: Ping-Hsun Wu <ping-hsun.wu@mediatek.com>
 */

#ifndef __MTK_MDP3_COMP_H__
#define __MTK_MDP3_COMP_H__

#include <linux/soc/mediatek/mtk-mmsys.h>
#include "mtk-mdp3-cmdq.h"

#define MM_REG_WRITE_MASK(cmd, id, base, ofst, val, mask, ...) \
	cmdq_pkt_write_mask((cmd)->pkt, id, \
		(base) + (ofst), (val), (mask), ##__VA_ARGS__)
#define MM_REG_WRITE(cmd, id, base, ofst, val, mask, ...) \
	MM_REG_WRITE_MASK(cmd, id, base, ofst, val, \
		(((mask) & (ofst##_MASK)) == (ofst##_MASK)) ? \
			(0xffffffff) : (mask), ##__VA_ARGS__)

#define MM_REG_WAIT(cmd, evt) \
	cmdq_pkt_wfe((cmd)->pkt, (cmd)->event[(evt)], true)

#define MM_REG_WAIT_NO_CLEAR(cmd, evt) \
	cmdq_pkt_wfe((cmd)->pkt, (cmd)->event[(evt)], false)

#define MM_REG_CLEAR(cmd, evt) \
	cmdq_pkt_clear_event((cmd)->pkt, (cmd)->event[(evt)])

#define MM_REG_SET_EVENT(cmd, evt) \
	cmdq_pkt_set_event((cmd)->pkt, (cmd)->event[(evt)])

#define MM_REG_POLL_MASK(cmd, id, base, ofst, val, mask, ...) \
	cmdq_pkt_poll_mask((cmd)->pkt, id, \
		(base) + (ofst), (val), (mask), ##__VA_ARGS__)
#define MM_REG_POLL(cmd, id, base, ofst, val, mask, ...) \
	MM_REG_POLL_MASK((cmd), id, base, ofst, val, \
		(((mask) & (ofst##_MASK)) == (ofst##_MASK)) ? \
			(0xffffffff) : (mask), ##__VA_ARGS__)

enum mdp_comp_type {
	MDP_COMP_TYPE_INVALID = 0,

	MDP_COMP_TYPE_IMGI,
	MDP_COMP_TYPE_WPEI,

	MDP_COMP_TYPE_RDMA,
	MDP_COMP_TYPE_RSZ,
	MDP_COMP_TYPE_WROT,
	MDP_COMP_TYPE_WDMA,
	MDP_COMP_TYPE_PATH1,
	MDP_COMP_TYPE_PATH2,
	MDP_COMP_TYPE_SPLIT,
	MDP_COMP_TYPE_STITCH,
	MDP_COMP_TYPE_FG,
	MDP_COMP_TYPE_OVL,
	MDP_COMP_TYPE_PAD,
	MDP_COMP_TYPE_MERGE,

	MDP_COMP_TYPE_TDSHP,
	MDP_COMP_TYPE_COLOR,
	MDP_COMP_TYPE_DRE,
	MDP_COMP_TYPE_CCORR,
	MDP_COMP_TYPE_HDR,
	MDP_COMP_TYPE_AAL,
	MDP_COMP_TYPE_TCC,

	MDP_COMP_TYPE_EXTO,	/* External path */
	MDP_COMP_TYPE_DL_PATH1, /* Direct-link path1 */
	MDP_COMP_TYPE_DL_PATH2, /* Direct-link path2 */
	MDP_COMP_TYPE_DL_PATH3, /* Direct-link path3 */
	MDP_COMP_TYPE_DL_PATH4, /* Direct-link path4 */
	MDP_COMP_TYPE_DL_PATH5, /* Direct-link path5 */
	MDP_COMP_TYPE_DL_PATH6, /* Direct-link path6 */
	MDP_COMP_TYPE_DUMMY,

	MDP_COMP_TYPE_COUNT	/* ALWAYS keep at the end */
};

enum mdp_comp_event {
	RDMA0_SOF,
	RDMA0_DONE,
	RSZ0_SOF,
	RSZ1_SOF,
	TDSHP0_SOF,
	WROT0_SOF,
	WROT0_DONE,
	WDMA0_SOF,
	WDMA0_DONE,
	RDMA1_SOF,
	RDMA2_SOF,
	RDMA3_SOF,
	WROT1_SOF,
	WROT2_SOF,
	WROT3_SOF,
	RDMA1_FRAME_DONE,
	RDMA2_FRAME_DONE,
	RDMA3_FRAME_DONE,
	WROT1_FRAME_DONE,
	WROT2_FRAME_DONE,
	WROT3_FRAME_DONE,

	ISP_P2_0_DONE,
	ISP_P2_1_DONE,
	ISP_P2_2_DONE,
	ISP_P2_3_DONE,
	ISP_P2_4_DONE,
	ISP_P2_5_DONE,
	ISP_P2_6_DONE,
	ISP_P2_7_DONE,
	ISP_P2_8_DONE,
	ISP_P2_9_DONE,
	ISP_P2_10_DONE,
	ISP_P2_11_DONE,
	ISP_P2_12_DONE,
	ISP_P2_13_DONE,
	ISP_P2_14_DONE,

	WPE_DONE,
	WPE_B_DONE,

	MDP_MAX_EVENT_COUNT	/* ALWAYS keep at the end */
};

enum mdp_mmsys_config_id {
	CONFIG_VPP0_HW_DCM_1ST_DIS0,
	CONFIG_VPP0_DL_IRELAY_WR,
	CONFIG_VPP1_HW_DCM_1ST_DIS0,
	CONFIG_VPP1_HW_DCM_1ST_DIS1,
	CONFIG_VPP1_HW_DCM_2ND_DIS0,
	CONFIG_VPP1_HW_DCM_2ND_DIS1,
	CONFIG_SVPP2_BUF_BF_RSZ_SWITCH,
	CONFIG_SVPP3_BUF_BF_RSZ_SWITCH,
	MDP_MAX_CONFIG_COUNT
};

struct mdp_comp_match {
	enum mdp_comp_type type;
	u32 alias_id;
	s32 inner_id;
};

/* Used to describe the item order in MDP property */
struct mdp_comp_info {
	u32 clk_num;
	u32 clk_ofst;
	u32 dts_reg_ofst;
};

struct mdp_mutex_info {
	u32 mmsys_id;
	u32 mod;
	u32 mod2;
};

struct mdp_comp_data {
	struct mdp_comp_match match;
	struct mdp_comp_info info;
	struct mdp_mutex_info mutex;
};

struct mdp_comp_ops;

struct mdp_comp {
	struct mdp_dev			*mdp_dev;
	void __iomem			*regs;
	phys_addr_t			reg_base;
	u8				subsys_id;
	struct clk			*clks[6];
	struct device			*comp_dev;
	enum mdp_comp_type		type;
	enum mtk_mdp_comp_id		public_id;
	u32				alias_id;
	s32				inner_id;
	const struct mdp_comp_ops	*ops;
};

struct mdp_comp_ctx {
	struct mdp_comp			*comp;
	const struct img_compparam	*param;
	const struct img_input		*input;
	const struct img_output		*outputs[IMG_MAX_HW_OUTPUTS];
};

struct mdp_comp_ops {
	s64 (*get_comp_flag)(const struct mdp_comp_ctx *ctx);
	int (*init_comp)(struct mdp_comp_ctx *ctx, struct mmsys_cmdq_cmd *cmd);
	int (*config_frame)(struct mdp_comp_ctx *ctx, struct mmsys_cmdq_cmd *cmd,
			    const struct v4l2_rect *compose);
	int (*config_subfrm)(struct mdp_comp_ctx *ctx,
			     struct mmsys_cmdq_cmd *cmd, u32 index);
	int (*wait_comp_event)(struct mdp_comp_ctx *ctx,
			       struct mmsys_cmdq_cmd *cmd);
	int (*advance_subfrm)(struct mdp_comp_ctx *ctx,
			      struct mmsys_cmdq_cmd *cmd, u32 index);
	int (*post_process)(struct mdp_comp_ctx *ctx, struct mmsys_cmdq_cmd *cmd);
};

struct mdp_dev;

s32 get_comp_inner_id(struct mdp_dev *mdp_dev, enum mtk_mdp_comp_id id);
enum mtk_mdp_comp_id get_comp_public_id(struct mdp_dev *mdp_dev, s32 id);
bool is_dummy_engine(struct mdp_dev *mdp_dev, s32 inner_id);
bool is_rdma(struct mdp_dev *mdp_dev, s32 inner_id);

int mdp_component_init(struct mdp_dev *mdp);
void mdp_component_deinit(struct mdp_dev *mdp);
void mdp_comp_clock_on(struct device *dev, struct mdp_comp *comp);
void mdp_comp_clock_off(struct device *dev, struct mdp_comp *comp);
void mdp_comp_clocks_on(struct device *dev, struct mdp_comp *comps, int num);
void mdp_comp_clocks_off(struct device *dev, struct mdp_comp *comps, int num);
int mdp_comp_ctx_init(struct mdp_dev *mdp, struct mdp_comp_ctx *ctx,
		      const struct img_compparam *param,
		      const struct img_ipi_frameparam *frame);

int mdp_get_event_idx(struct mdp_dev *mdp, enum mdp_comp_event event);

#endif  /* __MTK_MDP3_COMP_H__ */
