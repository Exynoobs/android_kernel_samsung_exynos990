/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#ifndef __DSP_HW_MEMORY_H__
#define __DSP_HW_MEMORY_H__

#include <linux/iommu.h>

#define DSP_MEMORY_MAX_SIZE	(SZ_16M)
#define DSP_CA5_BIN_IOVA	(0x10000000)
#define DSP_CA5_BIN_SIZE	(SZ_4M)

#define DSP_SM_BASE_ADDR	(0x50000)
#define DSP_SM(n)		(DSP_SM_BASE_ADDR + (n) * 4)

#define DSP_SM_RESERVED_SIZE	(SZ_256)
#define DSP_SM_RESERVED_COUNT	(DSP_SM_RESERVED_SIZE >> 2)
#define DSP_SM_RESERVED_BASE	(DSP_SM(1984))
#define DSP_SM_RESERVED(n)	(DSP_SM_RESERVED_BASE + 0x4 * (n))

struct dsp_priv_mem;

enum dsp_priv_mem_id {
	DSP_PRIV_MEM_FW,
	DSP_PRIV_MEM_IVP_PM,
	DSP_PRIV_MEM_DL_OUT,
	DSP_PRIV_MEM_COUNT,
};

struct dsp_memory {
	struct device		*dev;
	struct iommu_domain	*domain;
	struct dsp_priv_mem	*priv_mem;
};

#endif
