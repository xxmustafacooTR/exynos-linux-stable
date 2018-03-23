/*
 * SchedTune add-on features
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/sched.h>

/**********************************************************************
 *                            Prefer Perf                             *
 **********************************************************************/
int prefer_perf_cpu(struct task_struct *p)
{
	return -1;
}

/**********************************************************************
 *                            Prefer Idle                             *
 **********************************************************************/
int prefer_idle_cpu(struct task_struct *p)
{
	return -1;
}

/**********************************************************************
 *                           Group balancer                           *
 **********************************************************************/
int group_balancing(struct task_struct *p)
{
	return -1;
}
