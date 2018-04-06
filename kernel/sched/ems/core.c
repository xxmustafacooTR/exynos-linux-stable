/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#define CREATE_TRACE_POINTS
#include <trace/events/ems.h>

#include "ems.h"
#include "../sched.h"

int task_util(struct task_struct *p)
{
	return p->se.avg.util_avg;
}

int cpu_util_wake(int cpu, struct task_struct *p)
{
	unsigned long util, capacity;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !p->se.avg.last_update_time)
		return cpu_util(cpu);

	capacity = capacity_orig_of(cpu);
	util = max_t(long, cpu_rq(cpu)->cfs.avg.util_avg - task_util(p), 0);

	return (util >= capacity) ? capacity : util;
}

static inline int task_fits(struct task_struct *p, long capacity)
{
	return capacity * 1024 > task_util(p) * 1248;
}

struct sched_group *
exynos_fit_idlest_group(struct sched_domain *sd, struct task_struct *p)
{
	struct sched_group *group = sd->groups;
	struct sched_group *fit_group = NULL;
	unsigned long fit_capacity = ULONG_MAX;

	do {
		int i;

		/* Skip over this group if it has no CPUs allowed */
		if (!cpumask_intersects(sched_group_cpus(group),
					&p->cpus_allowed))
			continue;

		for_each_cpu(i, sched_group_cpus(group)) {
			if (capacity_of(i) < fit_capacity && task_fits(p, capacity_of(i))) {
				fit_capacity = capacity_of(i);
				fit_group = group;
			}
		}
	} while (group = group->next, group != sd->groups);

	return fit_group;
}

static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(rq->cpu_capacity_orig * 100));
}

#define lb_sd_parent(sd) \
	(sd->parent && sd->parent->groups != sd->parent->groups->next)

int exynos_need_active_balance(enum cpu_idle_type idle, struct sched_domain *sd,
					int src_cpu, int dst_cpu)
{
	unsigned int src_imb_pct = lb_sd_parent(sd) ? sd->imbalance_pct : 1;
	unsigned int dst_imb_pct = lb_sd_parent(sd) ? 100 : 1;
	unsigned long src_cap = capacity_of(src_cpu);
	unsigned long dst_cap = capacity_of(dst_cpu);
	int level = sd->level;

	/* dst_cpu is idle */
	if ((idle != CPU_NOT_IDLE) &&
	    (cpu_rq(src_cpu)->cfs.h_nr_running == 1)) {
		if ((check_cpu_capacity(cpu_rq(src_cpu), sd)) &&
		    (src_cap * sd->imbalance_pct < dst_cap * 100)) {
			return 1;
		}

		/* This domain is top and dst_cpu is bigger than src_cpu*/
		if (!lb_sd_parent(sd) && src_cap < dst_cap)
			if (lbt_overutilized(src_cpu, level) || global_boosted())
				return 1;
	}

	if ((src_cap * src_imb_pct < dst_cap * dst_imb_pct) &&
			cpu_rq(src_cpu)->cfs.h_nr_running == 1 &&
			lbt_overutilized(src_cpu, level) &&
			!lbt_overutilized(dst_cpu, level)) {
		return 1;
	}

	return unlikely(sd->nr_balance_failed > sd->cache_nice_tries + 2);
}

static int select_proper_cpu(struct task_struct *p)
{
	return -1;
}

extern void sync_entity_load_avg(struct sched_entity *se);

int exynos_wakeup_balance(struct task_struct *p, int prev_cpu, int sd_flag, int sync)
{
	int target_cpu = -1;
	char state[30] = "fail";

	/*
	 * Since the utilization of a task is accumulated before sleep, it updates
	 * the utilization to determine which cpu the task will be assigned to.
	 * Exclude new task.
	 */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	/*
	 * Priority 1 : ontime task
	 *
	 * If task which has more utilization than threshold wakes up, the task is
	 * classified as "ontime task" and assigned to performance cpu. Conversely,
	 * if heavy task that has been classified as ontime task sleeps for a long
	 * time and utilization becomes small, it is excluded from ontime task and
	 * is no longer guaranteed to operate on performance cpu.
	 *
	 * Ontime task is very sensitive to performance because it is usually the
	 * main task of application. Therefore, it has the highest priority.
	 */
	target_cpu = ontime_task_wakeup(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "ontime migration");
		goto out;
	}

	/*
	 * Priority 2 : prefer-perf
	 *
	 * Prefer-perf is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-perf is set to 1, the tasks in the group are
	 * preferentially assigned to the performance cpu.
	 *
	 * It has a high priority because it is a function that is turned on
	 * temporarily in scenario requiring reactivity(touch, app laucning).
	 */
	target_cpu = prefer_perf_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-perf");
		goto out;
	}

	/*
	 * Priority 3 : global boosting
	 *
	 * Global boost is a function that preferentially assigns all tasks in the
	 * system to the performance cpu. Unlike prefer-perf, which targets only
	 * group tasks, global boost targets all tasks. So, it maximizes performance
	 * cpu utilization.
	 *
	 * Typically, prefer-perf operates on groups that contains UX related tasks,
	 * such as "top-app" or "foreground", so that major tasks are likely to be
	 * assigned to performance cpu. On the other hand, global boost assigns
	 * all tasks to performance cpu, which is not as effective as perfer-perf.
	 * For this reason, global boost has a lower priority than prefer-perf.
	 */
	target_cpu = global_boosting(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "global boosting");
		goto out;
	}

	/*
	 * Priority 4 : group balancing
	 */
	target_cpu = group_balancing(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "group balancing");
		goto out;
	}

	/*
	 * Priority 5 : prefer-idle
	 *
	 * Prefer-idle is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-idle is set to 1, the tasks in the group are
	 * preferentially assigned to the idle cpu.
	 *
	 * Prefer-idle has a smaller performance impact than the above. Therefore
	 * it has a relatively low priority.
	 */
	target_cpu = prefer_idle_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-idle");
		goto out;
	}

	/*
	 * Priority 6 : energy cpu
	 *
	 * A scheduling scheme based on cpu energy, find the least power consumption
	 * cpu referring energy table when assigning task.
	 */
	target_cpu = select_energy_cpu(p, prev_cpu, sd_flag, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "energy cpu");
		goto out;
	}

	/*
	 * Priority 7 : proper cpu
	 */
	target_cpu = select_proper_cpu(p);
	if (cpu_selected(target_cpu))
		strcpy(state, "proper cpu");

out:
	trace_ems_wakeup_balance(p, target_cpu, state);
	return target_cpu;
}

struct kobject *ems_kobj;

static int __init init_sysfs(void)
{
	ems_kobj = kobject_create_and_add("ems", kernel_kobj);

	return 0;
}
core_initcall(init_sysfs);
