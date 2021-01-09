// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include <linux/platform_device.h>
#include <linux/io.h>

#include "dsp-log.h"
#include "dsp-device.h"
#include "dsp-binary.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-dump.h"

#define DSP_WAIT_BOOT_TIME	(100)

int dsp_system_request_control(struct dsp_system *sys, unsigned int id,
		union dsp_control *cmd)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_execute_task(struct dsp_system *sys, struct dsp_task *task)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_system_iovmm_fault_dump(struct dsp_system *sys)
{
	dsp_enter();
	dsp_dump_ctrl();
	dsp_leave();
}

int dsp_system_boot(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_reset(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_power_active(struct dsp_system *sys)
{
	dsp_check();
	return 0;
}

int dsp_system_set_boot_qos(struct dsp_system *sys, int val)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_runtime_resume(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_runtime_suspend(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_resume(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_suspend(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_npu_start(struct dsp_system *sys, bool boot, dma_addr_t fw_iova)
{
	return 0;
}

int dsp_system_start(struct dsp_system *sys, void *bin_list)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_stop(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_open(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_close(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_system_probe(struct dsp_device *dspdev)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_system_remove(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
}
