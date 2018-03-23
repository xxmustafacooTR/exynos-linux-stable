/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

extern struct kobject *ems_kobj;

extern int ontime_task_wakeup(struct task_struct *p);
extern int global_boosting(struct task_struct *p);

#ifdef CONFIG_SCHED_TUNE
extern int prefer_perf_cpu(struct task_struct *p);
extern int prefer_idle_cpu(struct task_struct *p);
extern int group_balancing(struct task_struct *p);
#else
static inline int prefer_perf_cpu(struct task_struct *p) { return -1; }
static inline int prefer_idle_cpu(struct task_struct *p) { return -1; }
static inline int group_balancing(struct task_struct *p) { return -1; }
#endif

#ifdef CONFIG_SCHED_EMS
extern int
exynos_wakeup_balance(struct task_struct *p, int sd_flag, int sync);
extern int __init lbt_sysfs_init(struct kobject *parent);
#else
static inline int
exynos_wakeup_balance(struct task_struct *p, int sd_flag, int sync)
{
	return -1;
}
static inline int __init lbt_sysfs_init(struct kobject *parent)
{
	return 0;
}
#endif
