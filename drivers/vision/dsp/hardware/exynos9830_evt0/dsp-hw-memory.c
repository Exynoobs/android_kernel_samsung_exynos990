// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include <linux/types.h>
#include <linux/ion_exynos.h>
#include <linux/exynos_iovmm.h>
#include <asm/cacheflush.h>

#include "dsp-log.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-memory.h"

int dsp_memory_map_buffer(struct dsp_memory *mem, struct dsp_buffer *buf)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_memory_unmap_buffer(struct dsp_memory *mem, struct dsp_buffer *buf)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_memory_sync_for_device(struct dsp_memory *mem, struct dsp_buffer *buf)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_memory_sync_for_cpu(struct dsp_memory *mem, struct dsp_buffer *buf)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_memory_ion_alloc(struct dsp_memory *mem, struct dsp_priv_mem *pmem)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_memory_ion_free(struct dsp_memory *mem, struct dsp_priv_mem *pmem)
{
	dsp_enter();
	dsp_leave();
}

int dsp_memory_open(struct dsp_memory *mem)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_memory_close(struct dsp_memory *mem)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_memory_probe(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_memory_remove(struct dsp_memory *mem)
{
	dsp_enter();
	dsp_leave();
}
