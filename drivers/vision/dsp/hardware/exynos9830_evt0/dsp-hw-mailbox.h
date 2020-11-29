/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#ifndef __DSP_HW_MAILBOX_H__
#define __DSP_HW_MAILBOX_H__

#include "dsp-util.h"

struct dsp_system;

struct dsp_mailbox {
	struct dsp_system		*sys;
};

#endif
