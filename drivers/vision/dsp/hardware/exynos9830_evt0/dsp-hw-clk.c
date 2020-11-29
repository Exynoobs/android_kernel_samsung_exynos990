// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include "dsp-log.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-clk.h"

void dsp_clk_dump(struct dsp_clk *clk)
{
	dsp_enter();
	dsp_leave();
}

void dsp_clk_user_dump(struct dsp_clk *clk, struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

int dsp_clk_enable(struct dsp_clk *clk)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_clk_disable(struct dsp_clk *clk)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_clk_open(struct dsp_clk *clk)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_clk_close(struct dsp_clk *clk)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_clk_probe(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_clk_remove(struct dsp_clk *clk)
{
	dsp_enter();
	dsp_leave();
}
