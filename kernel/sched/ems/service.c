/*
 * Services for Exynos Mobile Scheduler
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/kobject.h>
#include <linux/ems.h>

/**********************************************************************
 *                        Kernel Prefer Perf                          *
 **********************************************************************/
static atomic_t kernel_prefer_perf_req[STUNE_GROUP_COUNT];
int kernel_prefer_perf(int grp_idx)
{
	if (grp_idx >= STUNE_GROUP_COUNT)
		return -EINVAL;

	return atomic_read(&kernel_prefer_perf_req[grp_idx]);
}

void request_kernel_prefer_perf(int grp_idx, int value)
{
	if (grp_idx >= STUNE_GROUP_COUNT)
		return;

	atomic_set(&kernel_prefer_perf_req[grp_idx], value);
}

static ssize_t show_prefer_perf(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int i, ret = 0;

	/* shows the prefer_perf value of all schedtune groups */
	for (i = 0; i < STUNE_GROUP_COUNT; i++)
		ret += snprintf(buf + ret, 10, "%d ", kernel_prefer_perf(i));

	ret += snprintf(buf + ret, 10, "\n");

	return ret;
}

static struct kobj_attribute prefer_perf_attr =
__ATTR(kernel_prefer_perf, 0444, show_prefer_perf, NULL);

static int __init init_service(void)
{
	int ret;

	ret = sysfs_create_file(ems_kobj, &prefer_perf_attr.attr);
	if (ret)
		pr_err("%s: faile to create sysfs file\n", __func__);

	return 0;
}
late_initcall(init_service);
