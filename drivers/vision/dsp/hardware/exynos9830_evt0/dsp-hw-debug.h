/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#ifndef __DSP_HW_DEBUG_H__
#define __DSP_HW_DEBUG_H__

#include <linux/dcache.h>

struct dsp_device;

struct dsp_hw_debug {
	struct dentry		*root;
	struct dentry		*power;
	struct dentry		*clk;
	struct dentry		*devfreq;
	struct dentry		*sfr;
	struct dentry		*mem;
	struct dentry		*test;

	struct dsp_device	*dspdev;
};

#endif
