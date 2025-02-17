/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "dmub_abm.h"
#include "dce_abm.h"
#include "dc.h"
#include "dc_dmub_srv.h"
#include "../../dmub/inc/dmub_srv.h"
#include "core_types.h"
#include "dm_services.h"
#include "reg_helper.h"
#include "fixed31_32.h"

#include "atom.h"

#define TO_DMUB_ABM(abm)\
	container_of(abm, struct dce_abm, base)

#define REG(reg) \
	(dce_abm->regs->reg)

#undef FN
#define FN(reg_name, field_name) \
	dce_abm->abm_shift->field_name, dce_abm->abm_mask->field_name

#define CTX \
	dce_abm->base.ctx

#define DISABLE_ABM_IMMEDIATELY 255

static bool dmub_abm_set_pipe(struct abm *abm, uint32_t otg_inst)
{
	union dmub_rb_cmd cmd;
	struct dc_context *dc = abm->ctx;
	uint32_t ramping_boundary = 0xFFFF;

	cmd.abm_set_pipe.header.type = DMUB_CMD__ABM;
	cmd.abm_set_pipe.header.sub_type = DMUB_CMD__ABM_SET_PIPE;
	cmd.abm_set_pipe.abm_set_pipe_data.otg_inst = otg_inst;
	cmd.abm_set_pipe.abm_set_pipe_data.ramping_boundary = ramping_boundary;
	cmd.abm_set_pipe.header.payload_bytes = sizeof(struct dmub_cmd_abm_set_pipe_data);

	dc_dmub_srv_cmd_queue(dc->dmub_srv, &cmd.abm_set_pipe.header);
	dc_dmub_srv_cmd_execute(dc->dmub_srv);
	dc_dmub_srv_wait_idle(dc->dmub_srv);

	return true;
}

static unsigned int calculate_16_bit_backlight_from_pwm(struct dce_abm *dce_abm)
{
	uint64_t current_backlight;
	uint32_t round_result;
	uint32_t bl_period, bl_int_count;
	uint32_t bl_pwm, fractional_duty_cycle_en;
	uint32_t bl_period_mask, bl_pwm_mask;

	REG_GET(BL_PWM_PERIOD_CNTL, BL_PWM_PERIOD, &bl_period);
	REG_GET(BL_PWM_PERIOD_CNTL, BL_PWM_PERIOD_BITCNT, &bl_int_count);

	REG_GET(BL_PWM_CNTL, BL_ACTIVE_INT_FRAC_CNT, &bl_pwm);
	REG_GET(BL_PWM_CNTL, BL_PWM_FRACTIONAL_EN, &fractional_duty_cycle_en);

	if (bl_int_count == 0)
		bl_int_count = 16;

	bl_period_mask = (1 << bl_int_count) - 1;
	bl_period &= bl_period_mask;

	bl_pwm_mask = bl_period_mask << (16 - bl_int_count);

	if (fractional_duty_cycle_en == 0)
		bl_pwm &= bl_pwm_mask;
	else
		bl_pwm &= 0xFFFF;

	current_backlight = (uint64_t)bl_pwm << (1 + bl_int_count);

	if (bl_period == 0)
		bl_period = 0xFFFF;

	current_backlight = div_u64(current_backlight, bl_period);
	current_backlight = (current_backlight + 1) >> 1;

	current_backlight = (uint64_t)(current_backlight) * bl_period;

	round_result = (uint32_t)(current_backlight & 0xFFFFFFFF);

	round_result = (round_result >> (bl_int_count-1)) & 1;

	current_backlight >>= bl_int_count;
	current_backlight += round_result;

	return (uint32_t)(current_backlight);
}

static void dmcub_set_backlight_level(
	struct dce_abm *dce_abm,
	uint32_t backlight_pwm_u16_16,
	uint32_t frame_ramp,
	uint32_t otg_inst)
{
	union dmub_rb_cmd cmd;
	struct dc_context *dc = dce_abm->base.ctx;
	unsigned int backlight_8_bit = 0;
	uint32_t s2;

	if (backlight_pwm_u16_16 & 0x10000)
		// Check for max backlight condition
		backlight_8_bit = 0xFF;
	else
		// Take MSB of fractional part since backlight is not max
		backlight_8_bit = (backlight_pwm_u16_16 >> 8) & 0xFF;

	dmub_abm_set_pipe(&dce_abm->base, otg_inst);

	if (otg_inst == 0)
		frame_ramp = 0;

	cmd.abm_set_backlight.header.type = DMUB_CMD__ABM;
	cmd.abm_set_backlight.header.sub_type = DMUB_CMD__ABM_SET_BACKLIGHT;
	cmd.abm_set_backlight.abm_set_backlight_data.frame_ramp = frame_ramp;
	cmd.abm_set_backlight.header.payload_bytes = sizeof(struct dmub_cmd_abm_set_backlight_data);

	dc_dmub_srv_cmd_queue(dc->dmub_srv, &cmd.abm_set_backlight.header);
	dc_dmub_srv_cmd_execute(dc->dmub_srv);
	dc_dmub_srv_wait_idle(dc->dmub_srv);

	// Update requested backlight level
	s2 = REG_READ(BIOS_SCRATCH_2);

	s2 &= ~ATOM_S2_CURRENT_BL_LEVEL_MASK;
	backlight_8_bit &= (ATOM_S2_CURRENT_BL_LEVEL_MASK >>
				ATOM_S2_CURRENT_BL_LEVEL_SHIFT);
	s2 |= (backlight_8_bit << ATOM_S2_CURRENT_BL_LEVEL_SHIFT);

	REG_WRITE(BIOS_SCRATCH_2, s2);
}

static void dmub_abm_init(struct abm *abm)
{
	struct dce_abm *dce_abm = TO_DMUB_ABM(abm);
	unsigned int backlight = calculate_16_bit_backlight_from_pwm(dce_abm);

	REG_WRITE(DC_ABM1_HG_SAMPLE_RATE, 0x103);
	REG_WRITE(DC_ABM1_HG_SAMPLE_RATE, 0x101);
	REG_WRITE(DC_ABM1_LS_SAMPLE_RATE, 0x103);
	REG_WRITE(DC_ABM1_LS_SAMPLE_RATE, 0x101);
	REG_WRITE(BL1_PWM_BL_UPDATE_SAMPLE_RATE, 0x101);

	REG_SET_3(DC_ABM1_HG_MISC_CTRL, 0,
			ABM1_HG_NUM_OF_BINS_SEL, 0,
			ABM1_HG_VMAX_SEL, 1,
			ABM1_HG_BIN_BITWIDTH_SIZE_SEL, 0);

	REG_SET_3(DC_ABM1_IPCSC_COEFF_SEL, 0,
			ABM1_IPCSC_COEFF_SEL_R, 2,
			ABM1_IPCSC_COEFF_SEL_G, 4,
			ABM1_IPCSC_COEFF_SEL_B, 2);

	REG_UPDATE(BL1_PWM_CURRENT_ABM_LEVEL,
			BL1_PWM_CURRENT_ABM_LEVEL, backlight);

	REG_UPDATE(BL1_PWM_TARGET_ABM_LEVEL,
			BL1_PWM_TARGET_ABM_LEVEL, backlight);

	REG_UPDATE(BL1_PWM_USER_LEVEL,
			BL1_PWM_USER_LEVEL, backlight);

	REG_UPDATE_2(DC_ABM1_LS_MIN_MAX_PIXEL_VALUE_THRES,
			ABM1_LS_MIN_PIXEL_VALUE_THRES, 0,
			ABM1_LS_MAX_PIXEL_VALUE_THRES, 1000);

	REG_SET_3(DC_ABM1_HGLS_REG_READ_PROGRESS, 0,
			ABM1_HG_REG_READ_MISSED_FRAME_CLEAR, 1,
			ABM1_LS_REG_READ_MISSED_FRAME_CLEAR, 1,
			ABM1_BL_REG_READ_MISSED_FRAME_CLEAR, 1);
}

static unsigned int dmub_abm_get_current_backlight(struct abm *abm)
{
	struct dce_abm *dce_abm = TO_DMUB_ABM(abm);
	unsigned int backlight = REG_READ(BL1_PWM_CURRENT_ABM_LEVEL);

	/* return backlight in hardware format which is unsigned 17 bits, with
	 * 1 bit integer and 16 bit fractional
	 */
	return backlight;
}

static unsigned int dmub_abm_get_target_backlight(struct abm *abm)
{
	struct dce_abm *dce_abm = TO_DMUB_ABM(abm);
	unsigned int backlight = REG_READ(BL1_PWM_TARGET_ABM_LEVEL);

	/* return backlight in hardware format which is unsigned 17 bits, with
	 * 1 bit integer and 16 bit fractional
	 */
	return backlight;
}

static bool dmub_abm_set_level(struct abm *abm, uint32_t level)
{
	union dmub_rb_cmd cmd;
	struct dc_context *dc = abm->ctx;

	cmd.abm_set_level.header.type = DMUB_CMD__ABM;
	cmd.abm_set_level.header.sub_type = DMUB_CMD__ABM_SET_LEVEL;
	cmd.abm_set_level.abm_set_level_data.level = level;
	cmd.abm_set_level.header.payload_bytes = sizeof(struct dmub_cmd_abm_set_level_data);

	dc_dmub_srv_cmd_queue(dc->dmub_srv, &cmd.abm_set_level.header);
	dc_dmub_srv_cmd_execute(dc->dmub_srv);
	dc_dmub_srv_wait_idle(dc->dmub_srv);

	return true;
}

static bool dmub_abm_immediate_disable(struct abm *abm)
{
	struct dce_abm *dce_abm = TO_DMUB_ABM(abm);

	dmub_abm_set_pipe(abm, DISABLE_ABM_IMMEDIATELY);

	abm->stored_backlight_registers.BL_PWM_CNTL =
		REG_READ(BL_PWM_CNTL);
	abm->stored_backlight_registers.BL_PWM_CNTL2 =
		REG_READ(BL_PWM_CNTL2);
	abm->stored_backlight_registers.BL_PWM_PERIOD_CNTL =
		REG_READ(BL_PWM_PERIOD_CNTL);

	REG_GET(LVTMA_PWRSEQ_REF_DIV, BL_PWM_REF_DIV,
		&abm->stored_backlight_registers.LVTMA_PWRSEQ_REF_DIV_BL_PWM_REF_DIV);

	return true;
}

static void dmub_abm_enable_fractional_pwm(struct dc_context *dc)
{
	union dmub_rb_cmd cmd;
	uint32_t fractional_pwm = (dc->dc->config.disable_fractional_pwm == false) ? 1 : 0;

	cmd.abm_set_pwm_frac.header.type = DMUB_CMD__ABM;
	cmd.abm_set_pwm_frac.header.sub_type = DMUB_CMD__ABM_SET_PWM_FRAC;
	cmd.abm_set_pwm_frac.abm_set_pwm_frac_data.fractional_pwm = fractional_pwm;
	cmd.abm_set_pwm_frac.header.payload_bytes = sizeof(struct dmub_cmd_abm_set_pwm_frac_data);

	dc_dmub_srv_cmd_queue(dc->dmub_srv, &cmd.abm_set_pwm_frac.header);
	dc_dmub_srv_cmd_execute(dc->dmub_srv);
	dc_dmub_srv_wait_idle(dc->dmub_srv);
}

static bool dmub_abm_init_backlight(struct abm *abm)
{
	struct dce_abm *dce_abm = TO_DMUB_ABM(abm);
	uint32_t value;

	dmub_abm_enable_fractional_pwm(abm->ctx);

	/* It must not be 0, so we have to restore them
	 * Bios bug w/a - period resets to zero,
	 * restoring to cache values which is always correct
	 */
	REG_GET(BL_PWM_CNTL, BL_ACTIVE_INT_FRAC_CNT, &value);

	if (value == 0 || value == 1) {
		if (abm->stored_backlight_registers.BL_PWM_CNTL != 0) {
			REG_WRITE(BL_PWM_CNTL,
				abm->stored_backlight_registers.BL_PWM_CNTL);
			REG_WRITE(BL_PWM_CNTL2,
				abm->stored_backlight_registers.BL_PWM_CNTL2);
			REG_WRITE(BL_PWM_PERIOD_CNTL,
				abm->stored_backlight_registers.BL_PWM_PERIOD_CNTL);
			REG_UPDATE(LVTMA_PWRSEQ_REF_DIV,
				BL_PWM_REF_DIV,
				abm->stored_backlight_registers.LVTMA_PWRSEQ_REF_DIV_BL_PWM_REF_DIV);
		} else {
			/* TODO: Note: This should not really happen since VBIOS
			 * should have initialized PWM registers on boot.
			 */
			REG_WRITE(BL_PWM_CNTL, 0xC000FA00);
			REG_WRITE(BL_PWM_PERIOD_CNTL, 0x000C0FA0);
		}
	} else {
		abm->stored_backlight_registers.BL_PWM_CNTL =
				REG_READ(BL_PWM_CNTL);
		abm->stored_backlight_registers.BL_PWM_CNTL2 =
				REG_READ(BL_PWM_CNTL2);
		abm->stored_backlight_registers.BL_PWM_PERIOD_CNTL =
				REG_READ(BL_PWM_PERIOD_CNTL);

		REG_GET(LVTMA_PWRSEQ_REF_DIV, BL_PWM_REF_DIV,
				&abm->stored_backlight_registers.LVTMA_PWRSEQ_REF_DIV_BL_PWM_REF_DIV);
	}

	// Have driver take backlight control
	// TakeBacklightControl(true)
	value = REG_READ(BIOS_SCRATCH_2);
	value |= ATOM_S2_VRI_BRIGHT_ENABLE;
	REG_WRITE(BIOS_SCRATCH_2, value);

	// Enable the backlight output
	REG_UPDATE(BL_PWM_CNTL, BL_PWM_EN, 1);

	// Unlock group 2 backlight registers
	REG_UPDATE(BL_PWM_GRP1_REG_LOCK,
			BL_PWM_GRP1_REG_LOCK, 0);

	return true;
}

static bool dmub_abm_set_backlight_level_pwm(
		struct abm *abm,
		unsigned int backlight_pwm_u16_16,
		unsigned int frame_ramp,
		unsigned int otg_inst,
		bool use_smooth_brightness)
{
	struct dce_abm *dce_abm = TO_DMUB_ABM(abm);

	dmcub_set_backlight_level(dce_abm,
			backlight_pwm_u16_16,
			frame_ramp,
			otg_inst);

	return true;
}

static bool dmub_abm_load_config(struct abm *abm,
	unsigned int start_offset,
	const char *src,
	unsigned int bytes)
{
	return true;
}

static const struct abm_funcs abm_funcs = {
	.abm_init = dmub_abm_init,
	.set_abm_level = dmub_abm_set_level,
	.init_backlight = dmub_abm_init_backlight,
	.set_pipe = dmub_abm_set_pipe,
	.set_backlight_level_pwm = dmub_abm_set_backlight_level_pwm,
	.get_current_backlight = dmub_abm_get_current_backlight,
	.get_target_backlight = dmub_abm_get_target_backlight,
	.set_abm_immediate_disable = dmub_abm_immediate_disable,
	.load_abm_config = dmub_abm_load_config,
};

static void dmub_abm_construct(
	struct dce_abm *abm_dce,
	struct dc_context *ctx,
	const struct dce_abm_registers *regs,
	const struct dce_abm_shift *abm_shift,
	const struct dce_abm_mask *abm_mask)
{
	struct abm *base = &abm_dce->base;

	base->ctx = ctx;
	base->funcs = &abm_funcs;
	base->stored_backlight_registers.BL_PWM_CNTL = 0;
	base->stored_backlight_registers.BL_PWM_CNTL2 = 0;
	base->stored_backlight_registers.BL_PWM_PERIOD_CNTL = 0;
	base->stored_backlight_registers.LVTMA_PWRSEQ_REF_DIV_BL_PWM_REF_DIV = 0;
	base->dmcu_is_running = false;

	abm_dce->regs = regs;
	abm_dce->abm_shift = abm_shift;
	abm_dce->abm_mask = abm_mask;
}

struct abm *dmub_abm_create(
	struct dc_context *ctx,
	const struct dce_abm_registers *regs,
	const struct dce_abm_shift *abm_shift,
	const struct dce_abm_mask *abm_mask)
{
	struct dce_abm *abm_dce = kzalloc(sizeof(*abm_dce), GFP_KERNEL);

	if (abm_dce == NULL) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	dmub_abm_construct(abm_dce, ctx, regs, abm_shift, abm_mask);

	return &abm_dce->base;
}

void dmub_abm_destroy(struct abm **abm)
{
	struct dce_abm *abm_dce = TO_DMUB_ABM(*abm);

	kfree(abm_dce);
	*abm = NULL;
}
