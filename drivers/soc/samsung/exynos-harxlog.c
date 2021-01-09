/*
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *	      http://www.samsung.com/
 *
 * H-ARX Logging message
 * Author: SiheungKim <sheung.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/hvc.h>
#include <linux/jiffies.h>
#include <soc/samsung/exynos-harxlog.h>
#include <dt-bindings/soc/samsung/exynos9830-harxlog.h>
#include <linux/spinlock.h>
#include <asm/cacheflush.h>

/*
 * Macro for converting physical address to
 * virtual address that is mapped by vmap
 */
#define HARXLOG_PHYS_TO_VIRT(addr)		((unsigned long)logdata.virt_addr	\
						+ ((unsigned long)(addr)	\
						- logdata.phys_addr))

//static spinlock_t lock;
static struct harxlog_data_t logdata;
static struct harxlog_ctx_t hlog_ctx;
static struct harxlog_info_t *harxlog_log[NR_CPUS];
static unsigned int harxlog_initialized = 0;

static void *exynos_harxlog_request_region(unsigned long addr,
					unsigned int size)
{
	int i;
	unsigned int num_pages = (size >> PAGE_SHIFT);
	pgprot_t prot = pgprot_writecombine(PAGE_KERNEL);
	struct page **pages = NULL;
	void *v_addr = NULL;

	if (!addr)
		return NULL;

	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_ATOMIC);
	if (!pages)
		return NULL;

	for (i = 0; i < num_pages; i++) {
		pages[i] = phys_to_page(addr);
		addr += PAGE_SIZE;
	}

	v_addr = vmap(pages, num_pages, VM_MAP, prot);
	kfree(pages);

	return v_addr;
}

void exynos_harxlog_flush()
{
	if (hlog_ctx.enabled) {
		schedule_work(&hlog_ctx.work);
	}
	pr_debug("HARXLog sheduled!!\n");
}


void exynos_harxlog_worker(struct work_struct *work)
{
	struct log_header_info_t *v_log_h = NULL;
	char *v_log = NULL;
	unsigned long v_log_addr = 0;
	unsigned int cpu=0;
	unsigned int tmp_read_cnt = 0;
	unsigned int tmp_write_cnt = 0;

	if (!harxlog_initialized) {
		return;
	}

	for (cpu = 0; cpu < NR_CPUS ; cpu++) {
		v_log_addr = HARXLOG_PHYS_TO_VIRT(harxlog_log[cpu]->initial_log_addr);
		__inval_dcache_area((void *)harxlog_log[cpu],sizeof(struct log_header_info_t));
		tmp_read_cnt = harxlog_log[cpu]->log_read_cnt;
		tmp_write_cnt = harxlog_log[cpu]->log_write_cnt;
		pr_debug("[H-Arx LOG flush start] cpu id = %x\n",cpu);
		pr_debug("[H-Arx LOG flush start] write cnt = %x\n",harxlog_log[cpu]->log_write_cnt);
		pr_debug("[H-Arx LOG flush start] read cnt = %x\n",harxlog_log[cpu]->log_read_cnt);
		pr_debug("[H-Arx LOG flush start] overflow = %x\n",harxlog_log[cpu]->log_overflow);

		while (tmp_read_cnt != tmp_write_cnt) {
			v_log_h = (struct log_header_info_t *)v_log_addr;
			v_log = (char *)v_log_addr + sizeof(struct log_header_info_t);

			/* Print logs from EL2 */
			pr_info("[H-Arx LOG C%d] [%06d.%06d] %s",
				cpu,
				v_log_h->tv_sec,
				v_log_h->tv_usec,
				v_log);
			v_log_addr += (sizeof(struct log_header_info_t) + v_log_h->log_len);
			tmp_read_cnt = (tmp_read_cnt +1) % MAX_LOG_COUNT;
		}
		harxlog_log[cpu]->log_read_cnt = tmp_read_cnt;
		__flush_dcache_area((void *)harxlog_log[cpu],sizeof(struct log_header_info_t));
		if (harxlog_log[cpu]->log_overflow) {
			pr_info("[H-Arx Log Error C%d] H-Arx is full!!", cpu);
			harxlog_log[cpu]->log_overflow = 0;
		}

	}
	return ;
}

static int exynos_harxlog_probe(struct platform_device *pdev)
{
	unsigned long err;
	int i;
	pr_err("haxlog probing \n");
	/* Translate PA to VA of message buffer */
	logdata.virt_addr = exynos_harxlog_request_region(logdata.phys_addr, logdata.size);
	if (!logdata.virt_addr) {
		dev_err(&pdev->dev, "Fail to translate message buffer\n");
		return -EFAULT;
	}

	for (i = 0; i < NR_CPUS; i++) {
		harxlog_log[i] = (struct harxlog_info_t *)((unsigned long)logdata.virt_addr
								+ (HARXLOG_LOG_BUF_SIZE * i));
		dev_info(&pdev->dev,
			"harx_log[C%d]: %#lx\n",
			i, (unsigned long)harxlog_log[i]);
	}

	/* Init workqueue */
	INIT_WORK(&hlog_ctx.work, exynos_harxlog_worker);
	hlog_ctx.enabled = true;

	harxlog_initialized = 1;
	err = exynos_hvc(HVC_FID_BASE+0x30,HARXLOG_LOG_BUF_BASE,HARXLOG_LOG_BUF_SIZE,0,0);
	return 0;
}

static const struct of_device_id exynos_harxlog_of_match_table[] = {
	{ .compatible = "samsung,exynos-harxlog", },
	{ },
};

static struct platform_driver exynos_harxlog_driver = {
	.probe = exynos_harxlog_probe,
	.driver = {
		.name = "exynos-harxlog",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(exynos_harxlog_of_match_table),
	}
};

static int __init exynos_harxlog_reserved_mem_setup(struct reserved_mem *remem)
{
	logdata.phys_addr = remem->base;
	logdata.size = remem->size;

	pr_info("%s: Reserved memory for harxlog: addr=%lx, size=%lx\n",
			__func__, logdata.phys_addr, logdata.size);

	return 0;
}
RESERVEDMEM_OF_DECLARE(harxlog_mem, "exynos,harxlog", exynos_harxlog_reserved_mem_setup);


static int __init exynos_harxlog_init(void)
{
	pr_info("exynos harxlog_init \n");
	return platform_driver_register(&exynos_harxlog_driver);
}

static void __exit exynos_harxlog_exit(void)
{
	platform_driver_unregister(&exynos_harxlog_driver);
}

module_init(exynos_harxlog_init);
module_exit(exynos_harxlog_exit);

MODULE_DESCRIPTION("Exynos H-Arx log printing driver");
MODULE_AUTHOR("<sheung.kim@samsung.com>");
MODULE_LICENSE("GPL");

