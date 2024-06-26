/*
 * linux/drivers/gpu/exynos/g2d/g2d_regs.c
 *
 * Copyright (C) 2017 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/property.h>

#include "g2d.h"
#include "g2d_regs.h"
#include "g2d_task.h"
#include "g2d_uapi.h"
#include "g2d_debug.h"

#ifdef CONFIG_EXYNOS_CONTENT_PATH_PROTECTION
#include <linux/smc.h>
#include <asm/cacheflush.h>

struct g2d_task_secbuf {
	unsigned long cmd_paddr;
	int cmd_count;
	int priority;
	int job_id;
	int secure_layer;
};

void g2d_hw_push_task(struct g2d_device *g2d_dev, struct g2d_task *task)
{
	struct g2d_task_secbuf sec_task;
	int i;

	sec_task.cmd_paddr = (unsigned long)page_to_phys(task->cmd_page);
	sec_task.cmd_count = task->cmd_count;
	sec_task.priority = task->priority;
	sec_task.job_id = task->job_id;
	sec_task.secure_layer = 0;

	for (i = 0; i < task->num_source; i++) {
		if (!!(task->source[i].flags & G2D_LAYERFLAG_SECURE))
			sec_task.secure_layer |= 1 << i;
	}
	if (!!(task->target.flags & G2D_LAYERFLAG_SECURE) ||
		!!(sec_task.secure_layer))
		sec_task.secure_layer |= 1 << 24;

	__flush_dcache_area(&sec_task, sizeof(sec_task));
	__flush_dcache_area(page_address(task->cmd_page), G2D_CMD_LIST_SIZE);
	if (exynos_smc(SMC_DRM_G2D_CMD_DATA, virt_to_phys(&sec_task), 0, 0)) {
		dev_err(g2d_dev->dev, "%s : Failed to push %d %d %d %d\n",
			__func__, sec_task.cmd_count, sec_task.priority,
			sec_task.job_id, sec_task.secure_layer);

		BUG();
	}
}
#else
void g2d_hw_push_task(struct g2d_device *g2d_dev, struct g2d_task *task)
{
	u32 state = g2d_hw_get_job_state(g2d_dev, task->job_id);

	if (state != G2D_JOB_STATE_DONE)
		dev_err(g2d_dev->dev, "%s: Unexpected state %#x of JOB %d\n",
			__func__, state, task->job_id);

	if (device_get_dma_attr(g2d_dev->dev) != DEV_DMA_COHERENT)
		__flush_dcache_area(page_address(task->cmd_page),
				    G2D_CMD_LIST_SIZE);

	writel_relaxed(G2D_JOB_HEADER_DATA(task->priority, task->job_id),
			g2d_dev->reg + G2D_JOB_HEADER_REG);

	writel_relaxed(G2D_ERR_INT_ENABLE, g2d_dev->reg + G2D_INTEN_REG);

	writel_relaxed(task->cmd_addr, g2d_dev->reg + G2D_JOB_BASEADDR_REG);
	writel_relaxed(task->cmd_count, g2d_dev->reg + G2D_JOB_SFRNUM_REG);
	writel_relaxed(1 << task->job_id, g2d_dev->reg + G2D_JOB_INT_ID_REG);
	writel(G2D_JOBPUSH_INT_ENABLE, g2d_dev->reg + G2D_JOB_PUSH_REG);
}
#endif

bool g2d_hw_stuck_state(struct g2d_device *g2d_dev)
{
	int i, val;
	int retry_count = 10;

	while (retry_count-- > 0) {
		for (i = 0; i < G2D_MAX_JOBS; i++) {
			val = readl_relaxed(
				g2d_dev->reg + G2D_JOB_IDn_STATE_REG(i));

			val &= G2D_JOB_STATE_MASK;

			if ((i < MAX_SHARED_BUF_NUM) &&
				(val == G2D_JOB_STATE_RUNNING))
				return false;

			/* if every task are queued except hwfc job*/
			if ((i >= MAX_SHARED_BUF_NUM) &&
				(val != G2D_JOB_STATE_QUEUEING))
				return false;
		}
	}

	return true;
}

static const char *error_desc[3] = {
	"AFBC Stuck",
	"No read response",
	"No write response",
};

u32 g2d_hw_errint_status(struct g2d_device *g2d_dev)
{
	u32 status = readl_relaxed(g2d_dev->reg + G2D_INTC_PEND_REG);
	u32 errstatus = status;
	int idx;

	/* IRQPEND_SCF should not be set because we don't use ABP mode */
	BUG_ON((status & 0x1) == 1);

	errstatus >>= 16;
	errstatus &= 0x7;

	if (errstatus == 0)
		return 0;

	for (idx = 0; idx < 3; idx++) {
		if (errstatus & (1 << idx))
			dev_err(g2d_dev->dev,
				"G2D ERROR INTERRUPT: %s\n", error_desc[idx]);
	}

	dev_err(g2d_dev->dev, "G2D FIFO STATUS: %#x\n",
		g2d_hw_fifo_status(g2d_dev));

	return status;
}

/*
 * This is called when H/W is judged to be in operation,
 * for example, when a sysmmu fault and an error interrupt occurs.
 * If there is no task in progress, the status of all task on H/W
 * is taken and print for debugging
 */
int g2d_hw_get_current_task(struct g2d_device *g2d_dev)
{
	int i, val;

	for (i = 0; i < G2D_MAX_JOBS; i++) {
		val = readl_relaxed(g2d_dev->reg + G2D_JOB_IDn_STATE_REG(i));
		if ((val & G2D_JOB_STATE_MASK) == G2D_JOB_STATE_RUNNING)
			return i;
	}

	for (i = 0; i < G2D_MAX_JOBS; i++) {
		val = readl_relaxed(g2d_dev->reg + G2D_JOB_IDn_STATE_REG(i));
		dev_err(g2d_dev->dev,
			"G2D TASK[%03d] STATE : %d\n", i, val);
	}

	return -1;
}

void g2d_hw_kill_task(struct g2d_device *g2d_dev, unsigned int job_id)
{
	int retry_count = 120;

	writel((0 << 4) | job_id, g2d_dev->reg + G2D_JOB_KILL_REG);

	while (retry_count-- > 0) {
		if (!(readl(g2d_dev->reg + G2D_JOB_PUSHKILL_STATE_REG) & 0x2)) {
			dev_err(g2d_dev->dev,
				"%s: Killed JOB %d\n", __func__, job_id);
			return;
		}
	}

	dev_err(g2d_dev->dev, "%s: Failed to kill job %d\n", __func__, job_id);
}
