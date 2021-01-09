// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/syscalls.h>

#include "dsp-log.h"
#include "dsp-device.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-ctrl.h"
#include "hardware/dsp-dump.h"
#include "dsp-binary.h"
#include "dsp-debug.h"
#include "hardware/dsp-debug.h"

void dsp_hw_debug_log_flush(struct dsp_hw_debug *debug)
{
	dsp_enter();
	dsp_leave();
}

int dsp_hw_debug_log_start(struct dsp_hw_debug *debug)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_hw_debug_log_stop(struct dsp_hw_debug *debug)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_hw_debug_open(struct dsp_hw_debug *debug)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_hw_debug_close(struct dsp_hw_debug *debug)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_hw_debug_probe(struct dsp_device *dspdev)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_hw_debug_remove(struct dsp_hw_debug *debug)
{
	dsp_enter();
	dsp_leave();
}
