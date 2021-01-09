/*
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Device Tree binding constants for H-Arx Log
 */

#ifndef _EXYNOS_HARXLOG_TABLE_H_
#define _EXYNOS_HARXLOG_TABLE_H_

/*
 * NOTE
 *
 * HARXLOG_NUM_OF_CPU would be changed
 * at each SoC
 */
#define HARXLOG_NUM_OF_CPU			(8)

/* H-Arx log buffer information */
#define HARXLOG_LOG_BUF_BASE			(0xC3080000)
#define HARXLOG_LOG_BUF_SIZE			(0x10000)
#define HARXLOG_LOG_BUF_TOTAL_SIZE		(HARXLOG_LOG_BUF_SIZE * HARXLOG_NUM_OF_CPU)

#endif	/* _EXYNOS_HARXLOG_TABLE_H */
