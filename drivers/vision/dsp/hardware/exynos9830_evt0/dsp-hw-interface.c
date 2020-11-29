// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#include "dsp-log.h"
#include "hardware/dsp-ctrl.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-interface.h"

int dsp_interface_open(struct dsp_interface *itf)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_interface_close(struct dsp_interface *itf)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_interface_probe(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_interface_remove(struct dsp_interface *itf)
{
	dsp_enter();
	dsp_leave();
}
