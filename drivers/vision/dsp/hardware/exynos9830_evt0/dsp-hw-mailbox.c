// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos SoC series dsp driver
 *
 * Copyright (c) 2019 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 */

#include "dsp-log.h"
#include "hardware/dsp-system.h"
#include "hardware/dsp-mailbox.h"

struct dsp_mailbox_pool *dsp_mailbox_alloc_pool(struct dsp_mailbox *mbox,
		unsigned int size)
{
	dsp_enter();
	dsp_leave();
	return ERR_PTR(-EINVAL);
}

void dsp_mailbox_free_pool(struct dsp_mailbox_pool *pool)
{
	dsp_enter();
	dsp_leave();
}

int dsp_mailbox_send_message(struct dsp_mailbox *mbox, unsigned int message_id)
{
	dsp_enter();
	dsp_leave();
	return -EINVAL;
}

int dsp_mailbox_send_task(struct dsp_mailbox *mbox, struct dsp_task *task)
{
	dsp_enter();
	dsp_leave();
	return -EINVAL;
}

int dsp_mailbox_receive_task(struct dsp_mailbox *mbox)
{
	dsp_enter();
	dsp_leave();
	return -EINVAL;
}

int dsp_mailbox_start(struct dsp_mailbox *mbox)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_mailbox_stop(struct dsp_mailbox *mbox)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_mailbox_open(struct dsp_mailbox *mbox)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_mailbox_close(struct dsp_mailbox *mbox)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

int dsp_mailbox_probe(struct dsp_system *sys)
{
	dsp_enter();
	dsp_leave();
	return 0;
}

void dsp_mailbox_remove(struct dsp_mailbox *mbox)
{
	dsp_enter();
	dsp_leave();
}
