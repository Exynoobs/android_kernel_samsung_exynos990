// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/smc.h>

#include "dsp-log.h"
#include "hardware/dsp-reg.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-dump.h"
#include "dsp-device.h"
#include "dsp-debug.h"
#include "hardware/dsp-ctrl.h"

//#define ENABLE_SECURE_READ
//#define ENABLE_SECURE_WRITE

unsigned int dsp_ctrl_sm_readl(unsigned int reg_addr)
{
	dsp_check();
	return 0;
}

int dsp_ctrl_sm_writel(unsigned int reg_addr, int val)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

unsigned int dsp_ctrl_offset_readl(unsigned int reg_id, unsigned int offset)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_offset_writel(unsigned int reg_id, unsigned int offset, int val)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

unsigned int dsp_ctrl_readl(unsigned int reg_id)
{
	dsp_check();
	return 0;
}

int dsp_ctrl_writel(unsigned int reg_id, int val)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_common_init(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_init(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_all_init(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_start(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_reset(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_force_reset(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_ctrl_reg_print(unsigned int reg_id)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_pc_dump(void)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_reserved_sm_dump(void)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_userdefined_dump(void)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_dump(void)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_user_reg_print(struct seq_file *file, unsigned int reg_id)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_user_pc_dump(struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_user_reserved_sm_dump(struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_user_userdefined_dump(struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

void dsp_ctrl_user_dump(struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

int dsp_ctrl_open(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_close(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_ctrl_probe(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_ctrl_remove(struct dsp_ctrl *ctrl)
{
	dsp_enter();
	dsp_leave();
}
