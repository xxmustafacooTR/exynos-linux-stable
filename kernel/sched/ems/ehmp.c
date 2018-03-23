/*
 * Exynos scheduler for Heterogeneous Multi-Processing (HMP)
 *
 * Copyright (C) 2017 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/sched.h>
#include <linux/cpuidle.h>
#include <linux/pm_qos.h>
#include <linux/of.h>
#include <linux/ems.h>
#include <linux/sched_energy.h>

#include <trace/events/ems.h>

#include "../sched.h"
#include "../tune.h"
#include "ems.h"

/**********************************************************************
 * extern functions                                                   *
 **********************************************************************/
extern struct sched_entity *__pick_next_entity(struct sched_entity *se);
extern unsigned long boosted_task_util(struct task_struct *task);
extern unsigned long capacity_curr_of(int cpu);
extern int find_best_target(struct task_struct *p, int *backup_cpu,
				   bool boosted, bool prefer_idle);
extern u64 decay_load(u64 val, u64 n);
extern int start_cpu(bool boosted);

unsigned long task_util(struct task_struct *p)
{
	if (rt_task(p))
		return p->rt.avg.util_avg;
	else
		return p->se.avg.util_avg;
}

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static inline struct sched_entity *se_of(struct sched_avg *sa)
{
	return container_of(sa, struct sched_entity, avg);
}

static inline int task_fits(struct task_struct *p, long capacity)
{
	return capacity * 1024 > boosted_task_util(p) * 1248;
}

static unsigned long maxcap_val = 1024;
static int maxcap_cpu = 7;

void ehmp_update_max_cpu_capacity(int cpu, unsigned long val)
{
	maxcap_cpu = cpu;
	maxcap_val = val;
}

static inline struct device_node *get_ehmp_node(void)
{
	return of_find_node_by_path("/cpus/ehmp");
}

static inline struct cpumask *sched_group_cpus(struct sched_group *sg)
{
	return to_cpumask(sg->cpumask);
}

/**********************************************************************
 * Energy diff		                                              *
 **********************************************************************/
#define EAS_CPU_PRV	0
#define EAS_CPU_NXT	1
#define EAS_CPU_BKP	2

int exynos_estimate_idle_state(int cpu_idx, struct cpumask *mask,
				int state, int cpus)
{
	unsigned int deepest_state_residency = 0;
	unsigned int next_timer_us = 0;
	int grp_nr_running = 0;
	int deepest_state = 0;
	int i;
	int estimate_state = 0;

	if (cpu_idx == EAS_CPU_PRV)
		grp_nr_running++;

	for_each_cpu(i, mask) {
		grp_nr_running += cpu_rq(i)->nr_running;

		next_timer_us = ktime_to_us(tick_nohz_get_sleep_length_cpu(i));
		deepest_state_residency = cpuidle_get_target_residency(i, state);

		if (next_timer_us > deepest_state_residency)
			deepest_state++;
	}

	if (!grp_nr_running && deepest_state == cpus)
		estimate_state = state + 1;

	return estimate_state;
}

/**********************************************************************
 * task initialization                                                *
 **********************************************************************/
void exynos_init_entity_util_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = se->cfs_rq;
	struct sched_avg *sa = &se->avg;
	int cpu = cpu_of(cfs_rq->rq);
	unsigned long cap_org = capacity_orig_of(cpu);
	long cap = (long)(cap_org - cfs_rq->avg.util_avg) / 2;

	if (cap > 0) {
		if (cfs_rq->avg.util_avg != 0) {
			sa->util_avg  = cfs_rq->avg.util_avg * se->load.weight;
			sa->util_avg /= (cfs_rq->avg.load_avg + 1);

			if (sa->util_avg > cap)
				sa->util_avg = cap;
		} else {
			sa->util_avg = cap_org >> 2;
		}
		/*
		 * If we wish to restore tuning via setting initial util,
		 * this is where we should do it.
		 */
		sa->util_sum = sa->util_avg * LOAD_AVG_MAX;
	}
}

/**********************************************************************
 * load balance                                                       *
 **********************************************************************/
#define lb_sd_parent(sd) \
	(sd->parent && sd->parent->groups != sd->parent->groups->next)

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

/**********************************************************************
 * Global boost                                                       *
 **********************************************************************/
static unsigned long gb_value = 0;
static unsigned long gb_max_value = 0;
static struct gb_qos_request gb_req_user =
{
	.name = "ehmp_gb_req_user",
};

static struct plist_head gb_list = PLIST_HEAD_INIT(gb_list);

static DEFINE_SPINLOCK(gb_lock);

static int gb_qos_max_value(void)
{
	return plist_last(&gb_list)->prio;
}

static int gb_qos_req_value(struct gb_qos_request *req)
{
	return req->node.prio;
}

void __weak gb_qos_update_request(struct gb_qos_request *req, u32 new_value)
{
	unsigned long flags;

	if (req->node.prio == new_value)
		return;

	spin_lock_irqsave(&gb_lock, flags);

	if (req->active)
		plist_del(&req->node, &gb_list);
	else
		req->active = 1;

	plist_node_init(&req->node, new_value);
	plist_add(&req->node, &gb_list);

	gb_value = gb_max_value * gb_qos_max_value() / 100;
	trace_ehmp_global_boost(req->name, new_value);

	spin_unlock_irqrestore(&gb_lock, flags);
}

static ssize_t show_global_boost(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct gb_qos_request *req;
	int ret = 0;

	plist_for_each_entry(req, &gb_list, node)
		ret += snprintf(buf + ret, 30, "%s : %d\n",
				req->name, gb_qos_req_value(req));

	return ret;
}

static ssize_t store_global_boost(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	unsigned int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	gb_qos_update_request(&gb_req_user, input);

	return count;
}

static struct kobj_attribute global_boost_attr =
__ATTR(global_boost, 0644, show_global_boost, store_global_boost);

#define BOOT_BOOST_DURATION 40000000	/* microseconds */
unsigned long global_boost(void)
{
	u64 now = ktime_to_us(ktime_get());

	if (now < BOOT_BOOST_DURATION)
		return gb_max_value;

	return gb_value;
}

int find_second_max_cap(void)
{
	struct sched_domain *sd = rcu_dereference(per_cpu(sd_ea, 0));
	struct sched_group *sg;
	int max_cap = 0, second_max_cap = 0;

	if (!sd)
		return 0;

	sg = sd->groups;
	do {
		int i;

		for_each_cpu(i, sched_group_cpus(sg)) {
			if (max_cap < cpu_rq(i)->cpu_capacity_orig) {
				second_max_cap = max_cap;
				max_cap = cpu_rq(i)->cpu_capacity_orig;
			}
		}
	} while (sg = sg->next, sg != sd->groups);

	return second_max_cap;
}

static int __init init_global_boost(void)
{
	gb_max_value = find_second_max_cap() + 1;

	return 0;
}
pure_initcall(init_global_boost);

/**********************************************************************
 * Boost cpu selection (global boost, schedtune.prefer_perf)          *
 **********************************************************************/
int kernel_prefer_perf(int grp_idx);
static ssize_t show_prefer_perf(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int i, ret = 0;

	for (i = 0; i < STUNE_GROUP_COUNT; i++)
		ret += snprintf(buf + ret, 10, "%d ", kernel_prefer_perf(i));

	ret += snprintf(buf + ret, 10, "\n");

	return ret;
}

static struct kobj_attribute prefer_perf_attr =
__ATTR(kernel_prefer_perf, 0444, show_prefer_perf, NULL);

enum {
	BT_PREFER_PERF = 0,
	BT_GROUP_BALANCE,
	BT_GLOBAL_BOOST,
};

struct boost_trigger {
	int trigger;
	int boost_val;
};

static int check_boost_trigger(struct task_struct *p, struct boost_trigger *bt)
{
	int gb;

#ifdef CONFIG_SCHED_TUNE
	if (schedtune_prefer_perf(p) > 0) {
		bt->trigger = BT_PREFER_PERF;
		bt->boost_val = schedtune_perf_threshold();
		return 1;
	}

	if (schedtune_need_group_balance(p) > 0) {
		bt->trigger = BT_GROUP_BALANCE;
		bt->boost_val = schedtune_perf_threshold();
		return 1;
	}
#endif

	gb = global_boosted();
	if (gb) {
		bt->trigger = BT_GLOBAL_BOOST;
		bt->boost_val = gb;
		return 1;
	}

	/* not boost state */
	return 0;
}

static int boost_select_cpu(struct task_struct *p, struct cpumask *target_cpus)
{
	int i, cpu = 0;

	if (cpumask_empty(target_cpus))
		return -1;

	if (cpumask_test_cpu(task_cpu(p), target_cpus))
		return task_cpu(p);

	/* Return last cpu in target_cpus */
	for_each_cpu(i, target_cpus)
		cpu = i;

	return cpu;
}

static void mark_shallowest_cpu(int cpu, unsigned int *min_exit_latency,
						struct cpumask *shallowest_cpus)
{
	struct rq *rq = cpu_rq(cpu);
	struct cpuidle_state *idle = idle_get_state(rq);

	/* Before enabling cpuidle, all idle cpus are marked */
	if (!idle) {
		cpumask_set_cpu(cpu, shallowest_cpus);
		return;
	}

	/* Deeper idle cpu is ignored */
	if (idle->exit_latency > *min_exit_latency)
		return;

	/* if shallower idle cpu is found, previsouly founded cpu is ignored */
	if (idle->exit_latency < *min_exit_latency) {
		cpumask_clear(shallowest_cpus);
		*min_exit_latency = idle->exit_latency;
	}

	cpumask_set_cpu(cpu, shallowest_cpus);
}
static int check_migration_task(struct task_struct *p)
{
	return !p->se.avg.last_update_time;
}

unsigned long cpu_util_wake(int cpu, struct task_struct *p)
{
	unsigned long util, capacity;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || check_migration_task(p))
		return cpu_util(cpu);

	capacity = capacity_orig_of(cpu);
	util = max_t(long, cpu_util(cpu) - task_util(p), 0);

	return (util >= capacity) ? capacity : util;
}

static int find_group_boost_target(struct task_struct *p)
{
	struct sched_domain *sd;
	int shallowest_cpu = -1;
	int lowest_cpu = -1;
	unsigned int min_exit_latency = UINT_MAX;
	unsigned long lowest_util = ULONG_MAX;
	int target_cpu = -1;
	int cpu;
	char state[30] = "fail";

	sd = rcu_dereference(per_cpu(sd_ea, maxcap_cpu));
	if (!sd)
		return target_cpu;

	if (cpumask_test_cpu(task_cpu(p), sched_group_cpus(sd->groups))) {
		if (idle_cpu(task_cpu(p))) {
			target_cpu = task_cpu(p);
			strcpy(state, "current idle");
			goto find_target;
		}
	}

	for_each_cpu_and(cpu, tsk_cpus_allowed(p), sched_group_cpus(sd->groups)) {
		unsigned long util = cpu_util_wake(cpu, p);

		if (idle_cpu(cpu)) {
			struct cpuidle_state *idle;

			idle = idle_get_state(cpu_rq(cpu));
			if (!idle) {
				target_cpu = cpu;
				strcpy(state, "idle wakeup");
				goto find_target;
			}

			if (idle->exit_latency < min_exit_latency) {
				min_exit_latency = idle->exit_latency;
				shallowest_cpu = cpu;
				continue;
			}
		}

		if (cpu_selected(shallowest_cpu))
			continue;

		if (util < lowest_util) {
			lowest_cpu = cpu;
			lowest_util = util;
		}
	}

	if (cpu_selected(shallowest_cpu)) {
		target_cpu = shallowest_cpu;
		strcpy(state, "shallowest idle");
		goto find_target;
	}

	if (cpu_selected(lowest_cpu)) {
		target_cpu = lowest_cpu;
		strcpy(state, "lowest util");
	}

find_target:
	trace_ehmp_select_group_boost(p, target_cpu, state);

	return target_cpu;
}

static int
find_boost_target(struct sched_domain *sd, struct task_struct *p,
			unsigned long min_util, struct boost_trigger *bt)
{
	struct sched_group *sg;
	int boost = bt->boost_val;
	unsigned long max_capacity;
	struct cpumask boost_candidates;
	struct cpumask backup_boost_candidates;
	unsigned int min_exit_latency = UINT_MAX;
	unsigned int backup_min_exit_latency = UINT_MAX;
	int target_cpu;
	bool go_up = false;
	unsigned long lowest_util = ULONG_MAX;
	int lowest_cpu = -1;
	char state[30] = "fail";

	if (bt->trigger == BT_GROUP_BALANCE)
		return find_group_boost_target(p);

	cpumask_setall(&boost_candidates);
	cpumask_clear(&backup_boost_candidates);

	max_capacity = maxcap_val;

	sg = sd->groups;

	do {
		int i;

		for_each_cpu_and(i, tsk_cpus_allowed(p), sched_group_cpus(sg)) {
			unsigned long new_util, wake_util;

			if (!cpu_online(i))
				continue;

			wake_util = cpu_util_wake(i, p);
			new_util = wake_util + task_util(p);
			new_util = max(min_util, new_util);

			if (min(new_util + boost, max_capacity) > capacity_orig_of(i)) {
				if (!cpu_rq(i)->nr_running)
					mark_shallowest_cpu(i, &backup_min_exit_latency,
							&backup_boost_candidates);
				else if (cpumask_test_cpu(task_cpu(p), sched_group_cpus(sg)))
					go_up = true;

				continue;
			}

			if (cpumask_weight(&boost_candidates) >= nr_cpu_ids)
				cpumask_clear(&boost_candidates);

			if (!cpu_rq(i)->nr_running) {
				mark_shallowest_cpu(i, &min_exit_latency, &boost_candidates);
				continue;
			}

			if (wake_util < lowest_util) {
				lowest_util = wake_util;
				lowest_cpu = i;
			}
		}

		if (cpumask_weight(&boost_candidates) >= nr_cpu_ids)
			continue;

		target_cpu = boost_select_cpu(p, &boost_candidates);
		if (cpu_selected(target_cpu)) {
			strcpy(state, "big idle");
			goto out;
		}

		target_cpu = boost_select_cpu(p, &backup_boost_candidates);
		if (cpu_selected(target_cpu)) {
			strcpy(state, "little idle");
			goto out;
		}
	} while (sg = sg->next, sg != sd->groups);

	if (go_up) {
		strcpy(state, "lowest big cpu");
		target_cpu = lowest_cpu;
		goto out;
	}

	strcpy(state, "current cpu");
	target_cpu = task_cpu(p);

out:
	trace_ehmp_select_boost_cpu(p, target_cpu, bt->trigger, state);
	return target_cpu;
}

/**********************************************************************
 * schedtune.prefer_idle                                              *
 **********************************************************************/
static void mark_lowest_cpu(int cpu, unsigned long new_util,
			int *lowest_cpu, unsigned long *lowest_util)
{
	if (new_util >= *lowest_util)
		return;

	*lowest_util = new_util;
	*lowest_cpu = cpu;
}

static int find_prefer_idle_target(struct sched_domain *sd,
			struct task_struct *p, unsigned long min_util)
{
	struct sched_group *sg;
	int target_cpu = -1;
	int lowest_cpu = -1;
	int lowest_idle_cpu = -1;
	int overcap_cpu = -1;
	unsigned long lowest_util = ULONG_MAX;
	unsigned long lowest_idle_util = ULONG_MAX;
	unsigned long overcap_util = ULONG_MAX;
	struct cpumask idle_candidates;
	struct cpumask overcap_idle_candidates;

	cpumask_clear(&idle_candidates);
	cpumask_clear(&overcap_idle_candidates);

	sg = sd->groups;

	do {
		int i;

		for_each_cpu_and(i, tsk_cpus_allowed(p), sched_group_cpus(sg)) {
			unsigned long new_util, wake_util;

			if (!cpu_online(i))
				continue;

			wake_util = cpu_util_wake(i, p);
			new_util = wake_util + task_util(p);
			new_util = max(min_util, new_util);

			trace_ehmp_prefer_idle(p, task_cpu(p), i, task_util(p),
							new_util, idle_cpu(i));

			if (new_util > capacity_orig_of(i)) {
				if (idle_cpu(i)) {
					cpumask_set_cpu(i, &overcap_idle_candidates);
					mark_lowest_cpu(i, new_util,
						&overcap_cpu, &overcap_util);
				}

				continue;
			}

			if (idle_cpu(i)) {
				if (task_cpu(p) == i) {
					target_cpu = i;
					break;
				}

				cpumask_set_cpu(i, &idle_candidates);
				mark_lowest_cpu(i, new_util,
					&lowest_idle_cpu, &lowest_idle_util);

				continue;
			}

			mark_lowest_cpu(i, new_util, &lowest_cpu, &lowest_util);
		}

		if (cpu_selected(target_cpu))
			break;

		if (cpumask_weight(&idle_candidates)) {
			target_cpu = lowest_idle_cpu;
			break;
		}

		if (cpu_selected(lowest_cpu)) {
			target_cpu = lowest_cpu;
			break;
		}

	} while (sg = sg->next, sg != sd->groups);

	if (cpu_selected(target_cpu))
		goto out;

	if (cpumask_weight(&overcap_idle_candidates)) {
		if (cpumask_test_cpu(task_cpu(p), &overcap_idle_candidates))
			target_cpu = task_cpu(p);
		else
			target_cpu = overcap_cpu;

		goto out;
	}

out:
	trace_ehmp_prefer_idle_cpu_select(p, target_cpu);

	return target_cpu;
}

/**********************************************************************
 * cpu selection                                                      *
 **********************************************************************/

int exynos_select_cpu(struct task_struct *p, int *backup_cpu,
				bool boosted, bool prefer_idle)
{
	struct sched_domain *sd;
	int target_cpu = -1;
	int cpu;
	unsigned long min_util;
	struct boost_trigger trigger = {
		.trigger = 0,
		.boost_val = 0
	};

	target_cpu = ontime_task_wakeup(p);
	if (cpu_selected(target_cpu))
		goto exit;

	/* Find target cpu from lowest capacity domain */
	cpu = start_cpu(boosted);
	if (cpu < 0)
		goto exit;

	/* Find SD for the start CPU */
	sd = rcu_dereference(per_cpu(sd_ea, cpu));
	if (!sd)
		goto exit;

	min_util = boosted_task_util(p);

	if (check_boost_trigger(p, &trigger)) {
		target_cpu = find_boost_target(sd, p, min_util, &trigger);
		if (cpu_selected(target_cpu))
			goto exit;
	}

	if (prefer_idle) {
		target_cpu = find_prefer_idle_target(sd, p, min_util);
		if (cpu_selected(target_cpu))
			goto exit;
	}

	target_cpu = find_best_target(p, backup_cpu, 0, 0);

exit:

	return target_cpu;
}

/**********************************************************************
 * Sysfs                                                              *
 **********************************************************************/
static struct attribute *ehmp_attrs[] = {
	&global_boost_attr.attr,
	&prefer_perf_attr.attr,
	NULL,
};

static const struct attribute_group ehmp_group = {
	.attrs = ehmp_attrs,
};

static struct kobject *ehmp_kobj;

static int __init init_sysfs(void)
{
	int ret;

	ehmp_kobj = kobject_create_and_add("ehmp", kernel_kobj);
	ret = sysfs_create_group(ehmp_kobj, &ehmp_group);
	if (ret)
		return ret;

	return 0;
}
late_initcall(init_sysfs);
