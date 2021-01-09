/*
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *	      http://www.samsung.com/
 *
 * H-ARX Logging message Header
 * Author: SiheungKim, Junho choi <sheung.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */


#ifndef __EXYNOS_HARXLOG_H__
#define __EXYNOS_HARXLOG_H__

/* Error code */
#define ERROR_INVALID_LOG_LEN			(-1)
#define ERROR_INVALID_LOG_ADDR			(-2)
#define ERROR_INVALID_INTR_NUM			(-3)
#define ERROR_ALREADY_INITIALIZED		(-4)

#define LOG_PRINT_BUFFER_SIZE			(120)
#define MAX_LOG_COUNT				(510)

/* Alignment with 4 bytes */
#define FOUR_BYTES_SHIFT			(2)
#define FOUR_BYTES_MASK				((1 << FOUR_BYTES_SHIFT) - 1)

/*
 * If input address is not aligned with 4 bytes,
 * it makes this address be aligned with next 4 bytes.
 * Otherwise, there is no action.
 */
#define CHECK_AND_ALIGN_4BYTES(addr)		do {		\
		if ((addr) & FOUR_BYTES_MASK) {			\
			addr &= ~FOUR_BYTES_MASK;		\
			addr += (1 << FOUR_BYTES_SHIFT);	\
		}						\
	} while (0)

#ifndef __ASSEMBLY__
/* Reserved memory data */
struct harxlog_data_t {
	void *virt_addr;
	unsigned long phys_addr;
	unsigned long size;
};

/* Context for Harx log */
struct harxlog_ctx_t {
	struct work_struct work;
	/* debugfs root */
	struct dentry *debug_dir;
	/* seclog can be disabled via debugfs */
	bool enabled;
};



/* Log header information */
struct log_header_info_t {
	unsigned int log_len;
	unsigned int tv_sec;
	unsigned int tv_usec;
};

/* Harx log information */
struct harxlog_info_t {
	/* The count to write log */
	unsigned int log_write_cnt;
	/* The count to read log */
	unsigned int log_read_cnt;
	/*
	 * The log count when log_buf
	 * returns to initial_log_buf
	 */
	unsigned int log_return_cnt;
	/* Start log buffer address */
	unsigned long start_log_addr;
	/* Initial log buffer address */
	unsigned long initial_log_addr;
	/* log buffer overflow*/
	unsigned int log_overflow;
};
#endif	/* __ASSEMBLY__ */
void exynos_harxlog_flush(void);
#endif	/* __EXYNOS_HARXLOG_H__ */
