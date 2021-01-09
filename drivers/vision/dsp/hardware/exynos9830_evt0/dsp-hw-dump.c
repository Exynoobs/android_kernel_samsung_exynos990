// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include "dsp-log.h"
#include "dsp-kernel.h"
#include "hardware/dsp-ctrl.h"
#include "hardware/dsp-dump.h"
#include "hardware/dsp-mailbox.h"

void dsp_dump_set_value(unsigned int dump_value)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_print_value(void)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_print_status_user(struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_ctrl(void)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_ctrl_user(struct seq_file *file)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_mailbox_pool_error(struct dsp_mailbox_pool *pool)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_mailbox_pool_debug(struct dsp_mailbox_pool *pool)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_task_manager_count(struct dsp_task_manager *tmgr)
{
	dsp_enter();
	dsp_leave();
}

void dsp_dump_kernel(struct dsp_kernel_manager *kmgr)
{
	dsp_enter();
	dsp_leave();
}
