/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#ifndef __DSP_HW_SYSTEM_H__
#define __DSP_HW_SYSTEM_H__

#include <linux/device.h>
#include <linux/wait.h>

#include "hardware/dsp-pm.h"
#include "hardware/dsp-clk.h"
#include "hardware/dsp-memory.h"
#include "hardware/dsp-interface.h"
#include "hardware/dsp-ctrl.h"
#include "dsp-task.h"
#include "hardware/dsp-mailbox.h"
#include "hardware/dsp-debug.h"

struct dsp_system {
	struct device			*dev;
	phys_addr_t			sfr_pa;
	void __iomem			*sfr;
	resource_size_t			sfr_size;
	wait_queue_head_t		boot_wq;
	bool				boot_flag;

	struct dsp_pm			pm;
	struct dsp_clk			clk;
	struct dsp_memory		memory;
	struct dsp_interface		interface;
	struct dsp_ctrl			ctrl;
	struct dsp_task_manager		task_manager;
	struct dsp_mailbox		mailbox;
	struct dsp_hw_debug		debug;
};

#endif
