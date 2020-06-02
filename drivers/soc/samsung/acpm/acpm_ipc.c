/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/exynos-ss.h>

#include "acpm.h"
#include "acpm_ipc.h"
#include "fw_header/framework.h"

static struct acpm_ipc_info *acpm_ipc;
struct acpm_framework *acpm_initdata;
void __iomem *acpm_srambase;
struct regulator_ss_info regulator_ss[REGULATOR_SS_MAX];
char reg_map[0x100] = {0};
bool is_set_regmap = false;

static bool is_rt_dl_task_policy(void)
{
	return current->policy == SCHED_FIFO || current->policy == SCHED_RR
		|| current->policy == SCHED_DEADLINE;
}

void acpm_ipc_set_waiting_mode(bool mode)
{
	acpm_ipc->w_mode = mode;
}

struct regulator_ss_info *get_regulator_ss(int n)
{
	if (n < REGULATOR_SS_MAX)
		return &regulator_ss[n];
	else
		return NULL;
}

void set_reg_map(void)
{
	u32 idx;
	int i;

	for (i = 0; i < REGULATOR_SS_MAX; i++) {
		idx = regulator_ss[i].vsel_reg & 0xFF;
		if (idx == 0)
			continue;

		is_set_regmap = true;

		if (reg_map[idx] != 0)
			pr_err("duplicated set_reg_map [%d] reg_map %x\n", i, reg_map[idx]);

		reg_map[idx] = i;
	}
}

unsigned int get_reg_id(unsigned int addr)
{
	int id;

	if (addr >> 8 != 0x1)
		return NO_SS_RANGE;

	id = reg_map[addr & 0xff];
	if (id != 0)
		return id;

	return NO_SS_RANGE;
}

unsigned int get_reg_voltage(struct regulator_ss_info reg_info, unsigned int selector)
{
	if (reg_info.name[0] == 'L')
		selector = selector & 0x3F;

	return reg_info.min_uV + (reg_info.uV_step * (selector - reg_info.linear_min_sel));
}

int acpm_ipc_set_ch_mode(struct device_node *np, bool polling)
{
	int reg;
	int i, len, req_ch_id;
	const __be32 *prop;

	if (!np)
		return -ENODEV;

	prop = of_get_property(np, "acpm-ipc-channel", &len);
	if (!prop)
		return -ENOENT;
	req_ch_id = be32_to_cpup(prop);

	for(i = 0; i < acpm_ipc->num_channels; i++) {
		if (acpm_ipc->channel[i].id == req_ch_id) {

			reg = __raw_readl(acpm_ipc->intr + INTMR1);
			reg &= ~(1 << acpm_ipc->channel[i].id);
			reg |= polling << acpm_ipc->channel[i].id;
			__raw_writel(reg, acpm_ipc->intr + INTMR1);

			acpm_ipc->channel[i].polling = polling;

			return 0;
		}
	}

	return -ENODEV;
}

unsigned int acpm_ipc_request_channel(struct device_node *np, ipc_callback handler,
		unsigned int *id, unsigned int *size)
{
	struct callback_info *cb;
	int i, len, req_ch_id;
	const __be32 *prop;

	if (!np)
		return -ENODEV;

	prop = of_get_property(np, "acpm-ipc-channel", &len);
	if (!prop)
		return -ENOENT;
	req_ch_id = be32_to_cpup(prop);

	for(i = 0; i < acpm_ipc->num_channels; i++) {
		if (acpm_ipc->channel[i].id == req_ch_id) {
			*id = acpm_ipc->channel[i].id;
			*size = acpm_ipc->channel[i].tx_ch.size;

			if (handler) {
				cb = devm_kzalloc(acpm_ipc->dev, sizeof(struct callback_info),
						GFP_KERNEL);
				if (cb == NULL)
					return -ENOMEM;
				cb->ipc_callback = handler;
				cb->client = np;

				spin_lock(&acpm_ipc->channel[i].ch_lock);
				list_add(&cb->list, &acpm_ipc->channel[i].list);
				spin_unlock(&acpm_ipc->channel[i].ch_lock);
			}

			return 0;
		}
	}

	return -ENODEV;
}

unsigned int acpm_ipc_release_channel(struct device_node *np, unsigned int channel_id)
{
	struct acpm_ipc_ch *channel = &acpm_ipc->channel[channel_id];
	struct list_head *cb_list = &channel->list;
	struct callback_info *cb;

	list_for_each_entry(cb, cb_list, list) {
		if (cb->client == np) {
			spin_lock(&channel->ch_lock);
			list_del(&cb->list);
			spin_unlock(&channel->ch_lock);
			devm_kfree(acpm_ipc->dev, cb);
			break;
		}
	}

	return 0;
}

static bool check_response(struct acpm_ipc_ch *channel, struct ipc_config *cfg)
{
	unsigned int front;
	unsigned int rear;
	struct list_head *cb_list = &channel->list;
	struct callback_info *cb;
	unsigned int tmp_seq_num;
	bool ret = true;
	unsigned int i;

	spin_lock(&channel->rx_lock);
	/* IPC command dequeue */
	front = __raw_readl(channel->rx_ch.front);
	rear = __raw_readl(channel->rx_ch.rear);

	i = rear;

	while (i != front) {
		tmp_seq_num = __raw_readl(channel->rx_ch.base + channel->rx_ch.size * i);
		tmp_seq_num = (tmp_seq_num >> ACPM_IPC_PROTOCOL_SEQ_NUM) & 0x3f;

		if (tmp_seq_num == ((cfg->cmd[0] >> ACPM_IPC_PROTOCOL_SEQ_NUM) & 0x3f)) {
			memcpy_align_4(cfg->cmd, channel->rx_ch.base + channel->rx_ch.size * i,
					channel->rx_ch.size);
			memcpy_align_4(channel->cmd, channel->rx_ch.base + channel->rx_ch.size * i,
					channel->rx_ch.size);

			/* i: target command, rear: another command
			 * 1. i index command dequeue
			 * 2. rear index command copy to i index position
			 * 3. incresed rear index
			 */
			if (i != rear)
				memcpy_align_4(channel->rx_ch.base + channel->rx_ch.size * i,
						channel->rx_ch.base + channel->rx_ch.size * rear,
						channel->rx_ch.size);

			list_for_each_entry(cb, cb_list, list)
				if (cb && cb->ipc_callback)
					cb->ipc_callback(channel->cmd, channel->rx_ch.size);

			rear++;
			rear = rear % channel->rx_ch.len;

			__raw_writel(rear, channel->rx_ch.rear);
			front = __raw_readl(channel->rx_ch.front);

			if (rear == front) {
				__raw_writel((1 << channel->id), acpm_ipc->intr + INTCR1);
				if (rear != __raw_readl(channel->rx_ch.front)) {
					__raw_writel((1 << channel->id), acpm_ipc->intr + INTGR1);
				}
			}
			ret = false;
			channel->seq_num_flag[tmp_seq_num] = 0;
			break;
		}
		i++;
		i = i % channel->rx_ch.len;
	}

	spin_unlock(&channel->rx_lock);

	return ret;
}

static void dequeue_policy(struct acpm_ipc_ch *channel)
{
	unsigned int front;
	unsigned int rear;
	struct list_head *cb_list = &channel->list;
	struct callback_info *cb;

	spin_lock(&channel->rx_lock);

	if (channel->type == TYPE_BUFFER) {
		memcpy_align_4(channel->cmd, channel->rx_ch.base, channel->rx_ch.size);
		spin_unlock(&channel->rx_lock);
		list_for_each_entry(cb, cb_list, list)
			if (cb && cb->ipc_callback)
				cb->ipc_callback(channel->cmd, channel->rx_ch.size);

		return;
	}

	/* IPC command dequeue */
	front = __raw_readl(channel->rx_ch.front);
	rear = __raw_readl(channel->rx_ch.rear);

	while (rear != front) {
		memcpy_align_4(channel->cmd, channel->rx_ch.base + channel->rx_ch.size * rear,
				channel->rx_ch.size);

		list_for_each_entry(cb, cb_list, list)
			if (cb && cb->ipc_callback)
				cb->ipc_callback(channel->cmd, channel->rx_ch.size);

		if (channel->rx_ch.len == (rear + 1))
			rear = 0;
		else
			rear++;

		if (!channel->polling)
			complete(&channel->wait);

		__raw_writel(rear, channel->rx_ch.rear);
		front = __raw_readl(channel->rx_ch.front);
	}

	spin_unlock(&channel->rx_lock);
}

static irqreturn_t acpm_ipc_irq_handler(int irq, void *data)
{
	struct acpm_ipc_info *ipc = data;
	unsigned int status;
	int i;

	/* ACPM IPC INTERRUPT STATUS REGISTER */
	status = __raw_readl(acpm_ipc->intr + INTSR1);

	for (i = 0; i < acpm_ipc->num_channels; i++) {
		if (!ipc->channel[i].polling && (status & (0x1 << ipc->channel[i].id))) {
			/* ACPM IPC INTERRUPT PENDING CLEAR */
			__raw_writel(1 << ipc->channel[i].id, ipc->intr + INTCR1);
		}
	}

	ipc->intr_status = status;

	return IRQ_WAKE_THREAD;
}

static irqreturn_t acpm_ipc_irq_handler_thread(int irq, void *data)
{
	struct acpm_ipc_info *ipc = data;
	int i;

	for (i = 0; i < acpm_ipc->num_channels; i++)
		if (!ipc->channel[i].polling && (ipc->intr_status & (1 << i)))
			dequeue_policy(&ipc->channel[i]);

	return IRQ_HANDLED;
}

static void apm_interrupt_gen(unsigned int id)
{
	/* APM NVIC INTERRUPT GENERATE */
	writel((1 << id) << 16, acpm_ipc->intr + INTGR0);
}

static int enqueue_indirection_cmd(struct acpm_ipc_ch *channel,
		struct ipc_config *cfg)
{
	unsigned int front;
	unsigned int rear;
	unsigned int buf;
	bool timeout_flag = 0;

	if (cfg->indirection) {
		front = __raw_readl(channel->tx_ch.front);
		rear = __raw_readl(channel->tx_ch.rear);

		/* another indirection command check */
		while (rear != front) {
			buf = __raw_readl(channel->tx_ch.base + channel->tx_ch.size * rear);

			if (buf & (1 << ACPM_IPC_PROTOCOL_INDIRECTION)) {

				UNTIL_EQUAL(true, rear != __raw_readl(channel->tx_ch.rear),
						timeout_flag);

				if (timeout_flag) {
					return -ETIMEDOUT;
				} else {
					rear = __raw_readl(channel->tx_ch.rear);
				}

			} else {
				if (channel->tx_ch.len == (rear + 1))
					rear = 0;
				else
					rear++;
			}
		}

		if (cfg->indirection_base)
			memcpy_align_4(channel->tx_ch.direction, cfg->indirection_base,
					cfg->indirection_size);
		else
			return -EINVAL;
	}

	return 0;
}

int acpm_ipc_send_data_sync(unsigned int channel_id, struct ipc_config *cfg)
{
	int ret;
	struct acpm_ipc_ch *channel;

	ret = acpm_ipc_send_data(channel_id, cfg);

	if (!ret) {
		channel = &acpm_ipc->channel[channel_id];

		if (!channel->polling && cfg->response) {
			ret = wait_for_completion_interruptible_timeout(&channel->wait,
					msecs_to_jiffies(50));
			if (!ret) {
				pr_err("[%s] ipc_timeout!!!\n", __func__);
				ret = -ETIMEDOUT;
			} else {
				ret = 0;
			}
		}
	}

	return ret;
}

int __acpm_ipc_send_data(unsigned int channel_id, struct ipc_config *cfg, bool w_mode)
{
	unsigned int front;
	unsigned int rear;
	unsigned int tmp_index;
	struct acpm_ipc_ch *channel;
	bool timeout_flag = 0;
	int ret;
	u64 timeout, now;
	u32 retry_cnt = 0;
	u32 tmp_seq_num;
	u32 seq_cnt = 0;

	if (channel_id >= acpm_ipc->num_channels && !cfg)
		return -EIO;

	channel = &acpm_ipc->channel[channel_id];

	spin_lock(&channel->tx_lock);

	front = __raw_readl(channel->tx_ch.front);
	rear = __raw_readl(channel->tx_ch.rear);

	tmp_index = front + 1;

	if (tmp_index >= channel->tx_ch.len)
		tmp_index = 0;

	/* buffer full check */
	UNTIL_EQUAL(true, tmp_index != __raw_readl(channel->tx_ch.rear), timeout_flag);
	if (timeout_flag) {
		spin_unlock(&channel->tx_lock);
		pr_err("[%s] tx buffer full! timeout!!!\n", __func__);
		return -ETIMEDOUT;
	}

	if (!cfg->cmd) {
		spin_unlock(&channel->tx_lock);
		return -EIO;
	}

	tmp_seq_num = channel->seq_num;
	do {
		if (unlikely(tmp_seq_num != channel->seq_num)) {
			pr_warn("ACPM IPC] [ACPM_IPC] channel:%d, cmd:0x%x, 0x%x, 0x%x, 0x%x",
					channel->id, cfg->cmd[0], cfg->cmd[1],
					cfg->cmd[2], cfg->cmd[3]);
			WARN(1, "[ACPM IPC] duplicate assignment: sequence number:%d, tmp_seq_num:%d, flag:0x%x",
					channel->seq_num, tmp_seq_num, channel->seq_num_flag[tmp_seq_num]);
		}

		if (++tmp_seq_num == SEQUENCE_NUM_MAX)
			tmp_seq_num = 1;
		
		if (unlikely(seq_cnt++ == SEQUENCE_NUM_MAX)) {
			pr_err("[ACPM IPC] sequence number full! error!!!\n");
			BUG();
		}
	} while (channel->seq_num_flag[tmp_seq_num]);

	channel->seq_num = tmp_seq_num;
	channel->seq_num_flag[channel->seq_num] = cfg->cmd[0] | (0x1 << 31);

	cfg->cmd[0] &= ~(0x3f << ACPM_IPC_PROTOCOL_SEQ_NUM);
	cfg->cmd[0] |= (channel->seq_num & 0x3f) << ACPM_IPC_PROTOCOL_SEQ_NUM;

	memcpy_align_4(channel->tx_ch.base + channel->tx_ch.size * front, cfg->cmd,
			channel->tx_ch.size);

	cfg->cmd[1] = 0;
	cfg->cmd[2] = 0;
	cfg->cmd[3] = 0;

	ret = enqueue_indirection_cmd(channel, cfg);
	if (ret) {
		pr_err("[ACPM] indirection command fail %d\n", ret);
		spin_unlock(&channel->tx_lock);
		return ret;
	}

	writel(tmp_index, channel->tx_ch.front);

	apm_interrupt_gen(channel->id);
	spin_unlock(&channel->tx_lock);

	if (channel->polling && cfg->response) {
retry:
		timeout = sched_clock() + IPC_TIMEOUT;
		timeout_flag = false;

		while (!(__raw_readl(acpm_ipc->intr + INTSR1) & (1 << channel->id)) ||
				check_response(channel, cfg)) {
			now = sched_clock();
			if (timeout < now) {
				if (retry_cnt > 5) {
					timeout_flag = true;
					break;
				} else if (retry_cnt > 0) {
					pr_err("acpm_ipc timeout retry %d "
						"now = %llu,"
						"timeout = %llu\n",
						retry_cnt, now, timeout);
					++retry_cnt;
					goto retry;
				} else {
					++retry_cnt;
					continue;
				}
			} else {
				if (w_mode)
					usleep_range(50, 100);
				else
					udelay(10);
			}
		}

		if (timeout_flag) {
			if (!check_response(channel, cfg))
				return 0;
			pr_err("%s Timeout error! now = %llu, timeout = %llu\n",
					__func__, now, timeout);
			pr_err("[ACPM] int_status:0x%x, ch_id: 0x%x\n",
					__raw_readl(acpm_ipc->intr + INTSR1),
					1 << channel->id);
			pr_err("[ACPM] queue, rx_rear:%u, rx_front:%u\n",
					__raw_readl(channel->rx_ch.rear),
					__raw_readl(channel->rx_ch.front));
			pr_err("[ACPM] queue, tx_rear:%u, tx_front:%u\n",
					__raw_readl(channel->tx_ch.rear),
					__raw_readl(channel->tx_ch.front));

			msleep(1000);
			s3c2410wdt_set_emergency_reset(0, 0);
		}
	}

	return 0;
}

int acpm_ipc_send_data(unsigned int channel_id, struct ipc_config *cfg)
{
	int ret;

	ret = __acpm_ipc_send_data(channel_id, cfg, false);

	return ret;
}

int acpm_ipc_send_data_lazy(unsigned int channel_id, struct ipc_config *cfg)
{
	int ret;

	if (is_rt_dl_task_policy())
		ret = __acpm_ipc_send_data(channel_id, cfg, true);
	else
		ret = __acpm_ipc_send_data(channel_id, cfg, false);

	return ret;
}

static int channel_init(void)
{
	int i;
	unsigned int mask = 0;
	struct ipc_channel *ipc_ch;

	acpm_ipc->num_channels = acpm_ipc->initdata->ipc_ap_max;

	acpm_ipc->channel = devm_kzalloc(acpm_ipc->dev,
			sizeof(struct acpm_ipc_ch) * acpm_ipc->num_channels, GFP_KERNEL);

	for (i = 0; i < acpm_ipc->num_channels; i++) {
		ipc_ch = (struct ipc_channel *)(acpm_ipc->sram_base + acpm_ipc->initdata->ipc_channels);
		acpm_ipc->channel[i].polling = ipc_ch[i].ap_poll;
		acpm_ipc->channel[i].id = ipc_ch[i].id;
		acpm_ipc->channel[i].type = ipc_ch[i].type;
		mask |= acpm_ipc->channel[i].polling << acpm_ipc->channel[i].id;

		/* Channel's RX buffer info */
		acpm_ipc->channel[i].rx_ch.size = ipc_ch[i].ch.q_elem_size;
		acpm_ipc->channel[i].rx_ch.len = ipc_ch[i].ch.q_len;
		acpm_ipc->channel[i].rx_ch.rear = acpm_ipc->sram_base + ipc_ch[i].ch.tx_rear;
		acpm_ipc->channel[i].rx_ch.front = acpm_ipc->sram_base + ipc_ch[i].ch.tx_front;
		acpm_ipc->channel[i].rx_ch.base = acpm_ipc->sram_base + ipc_ch[i].ch.tx_base;
		/* Channel's TX buffer info */
		acpm_ipc->channel[i].tx_ch.size = ipc_ch[i].ch.q_elem_size;
		acpm_ipc->channel[i].tx_ch.len = ipc_ch[i].ch.q_len;
		acpm_ipc->channel[i].tx_ch.rear = acpm_ipc->sram_base + ipc_ch[i].ch.rx_rear;
		acpm_ipc->channel[i].tx_ch.front = acpm_ipc->sram_base + ipc_ch[i].ch.rx_front;
		acpm_ipc->channel[i].tx_ch.base = acpm_ipc->sram_base + ipc_ch[i].ch.rx_base;
		acpm_ipc->channel[i].tx_ch.d_buff_size = ipc_ch[i].ch.rx_indr_buf_size;
		acpm_ipc->channel[i].tx_ch.direction = acpm_ipc->sram_base + ipc_ch[i].ch.rx_indr_buf;

		acpm_ipc->channel[i].cmd = devm_kzalloc(acpm_ipc->dev,
				acpm_ipc->channel[i].tx_ch.size, GFP_KERNEL);

		init_completion(&acpm_ipc->channel[i].wait);
		spin_lock_init(&acpm_ipc->channel[i].rx_lock);
		spin_lock_init(&acpm_ipc->channel[i].tx_lock);
		spin_lock_init(&acpm_ipc->channel[i].ch_lock);
		INIT_LIST_HEAD(&acpm_ipc->channel[i].list);
	}

	__raw_writel(mask, acpm_ipc->intr + INTMR1);

	return 0;
}

static int acpm_ipc_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	int ret = 0, len;
	const __be32 *prop;

	if (!node) {
		dev_err(&pdev->dev, "driver doesnt support"
				"non-dt devices\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "acpm_ipc probe\n");

	acpm_ipc = devm_kzalloc(&pdev->dev,
			sizeof(struct acpm_ipc_info), GFP_KERNEL);

	if (IS_ERR(acpm_ipc))
		return PTR_ERR(acpm_ipc);

	acpm_ipc->irq = irq_of_parse_and_map(node, 0);

	ret = devm_request_threaded_irq(&pdev->dev, acpm_ipc->irq, acpm_ipc_irq_handler,
			acpm_ipc_irq_handler_thread,
			IRQF_ONESHOT,
			dev_name(&pdev->dev), acpm_ipc);

	if (ret) {
		dev_err(&pdev->dev, "failed to register acpm_ipc interrupt:%d\n", ret);
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	acpm_ipc->intr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(acpm_ipc->intr))
		return PTR_ERR(acpm_ipc->intr);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	acpm_ipc->sram_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(acpm_ipc->sram_base))
		return PTR_ERR(acpm_ipc->sram_base);

	prop = of_get_property(node, "initdata-base", &len);
	if (prop) {
		acpm_ipc->initdata_base = be32_to_cpup(prop);
	} else {
		dev_err(&pdev->dev, "Parsing initdata_base failed.\n");
		return -EINVAL;
	}
	acpm_ipc->initdata = (struct acpm_framework *)(acpm_ipc->sram_base + acpm_ipc->initdata_base);
	acpm_initdata = acpm_ipc->initdata;
	acpm_srambase = acpm_ipc->sram_base;

	acpm_ipc->dev = &pdev->dev;

	channel_init();

	return ret;
}

static int acpm_ipc_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id acpm_ipc_match[] = {
	{ .compatible = "samsung,exynos-acpm-ipc" },
	{},
};

static struct platform_driver samsung_acpm_ipc_driver = {
	.probe	= acpm_ipc_probe,
	.remove	= acpm_ipc_remove,
	.driver	= {
		.name = "exynos-acpm-ipc",
		.owner	= THIS_MODULE,
		.of_match_table	= acpm_ipc_match,
	},
};

static int __init exynos_acpm_ipc_init(void)
{
	return platform_driver_register(&samsung_acpm_ipc_driver);
}
arch_initcall(exynos_acpm_ipc_init);
