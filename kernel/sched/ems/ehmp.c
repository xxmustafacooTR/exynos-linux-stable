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

#define entity_is_cfs_rq(se)	(se->my_q)
#define entity_is_task(se)	(!se->my_q)
#define LOAD_AVG_MAX		47742

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

#define tsk_cpus_allowed(tsk)	(&(tsk)->cpus_allowed)

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

unsigned long global_boost(void);
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
			if (lbt_overutilized(src_cpu, level) || global_boost())
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

/****************************************************************/
/*			Load Balance Trigger			*/
/****************************************************************/
#define DISABLE_OU		-1
#define DEFAULT_OU_RATIO	80

struct lbt_overutil {
	bool			top;
	struct cpumask		cpus;
	unsigned long		capacity;
	int			ratio;
};
DEFINE_PER_CPU(struct lbt_overutil *, lbt_overutil);

static inline int get_topology_depth(void)
{
	struct sched_domain *sd;

	for_each_domain(0, sd) {
		if (sd->parent == NULL)
			return sd->level;
	}

	return -1;
}

static inline int get_last_level(struct lbt_overutil *ou)
{
	int level;

	for (level = 0; &ou[level] != NULL; level++) {
		if (ou[level].top == true)
			return level;
	}

	return -1;
}

/****************************************************************/
/*			External APIs				*/
/****************************************************************/
bool lbt_overutilized(int cpu, int level)
{
	struct lbt_overutil *ou = per_cpu(lbt_overutil, cpu);
	bool overutilized;

	if (!ou)
		return false;

	overutilized = (cpu_util(cpu) > ou[level].capacity) ? true : false;

	if (overutilized)
		trace_ehmp_lbt_overutilized(cpu, level, cpu_util(cpu),
				ou[level].capacity, overutilized);

	return overutilized;
}

void update_lbt_overutil(int cpu, unsigned long capacity)
{
	struct lbt_overutil *ou = per_cpu(lbt_overutil, cpu);
	int level, last = get_last_level(ou);

	for (level = 0; level <= last; level++) {
		if (ou[level].ratio == DISABLE_OU)
			continue;

		ou[level].capacity = (capacity * ou[level].ratio) / 100;
	}
}

/****************************************************************/
/*				SYSFS				*/
/****************************************************************/
#define lbt_attr_init(_attr, _name, _mode, _show, _store)		\
	sysfs_attr_init(&_attr.attr);					\
	_attr.attr.name = _name;					\
	_attr.attr.mode = VERIFY_OCTAL_PERMISSIONS(_mode);		\
	_attr.show	= _show;					\
	_attr.store	= _store;

static struct kobject *lbt_kobj;
static struct attribute **lbt_attrs;
static struct kobj_attribute *lbt_kattrs;
static struct attribute_group lbt_group;

static ssize_t show_overutil_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct lbt_overutil *ou = per_cpu(lbt_overutil, 0);
	int level = attr - lbt_kattrs;
	int cpu, ret = 0;

	for_each_possible_cpu(cpu) {
		ou = per_cpu(lbt_overutil, cpu);

		if (ou[level].ratio == DISABLE_OU)
			continue;

		ret += sprintf(buf + ret, "cpu%d ratio:%3d capacity:%4lu\n",
				cpu, ou[level].ratio, ou[level].capacity);
	}

	return ret;
}

static ssize_t store_overutil_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	struct lbt_overutil *ou;
	unsigned long capacity;
	int level = attr - lbt_kattrs;
	int cpu, ratio;

	if (sscanf(buf, "%d %d", &cpu, &ratio) != 2)
		return -EINVAL;

	/* Check cpu is possible */
	if (!cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;
	ou = per_cpu(lbt_overutil, cpu);

	/* If ratio is outrage, disable overutil */
	if (ratio < 0 || ratio > 100)
		ratio = DEFAULT_OU_RATIO;

	for_each_cpu(cpu, &ou[level].cpus) {
		ou = per_cpu(lbt_overutil, cpu);
		if (ou[level].ratio == DISABLE_OU)
			continue;

		ou[level].ratio = ratio;
		capacity = capacity_orig_of(cpu);
		update_lbt_overutil(cpu, capacity);
	}

	return count;
}

static int alloc_lbt_sysfs(int size)
{
	if (size < 0)
		return -EINVAL;

	lbt_attrs = kzalloc(sizeof(struct attribute *) * (size + 1),
			GFP_KERNEL);
	if (!lbt_attrs)
		goto fail_alloc;

	lbt_kattrs = kzalloc(sizeof(struct kobj_attribute) * (size),
			GFP_KERNEL);
	if (!lbt_kattrs)
		goto fail_alloc;

	return 0;

fail_alloc:
	kfree(lbt_attrs);
	kfree(lbt_kattrs);

	pr_err("LBT(%s): failed to alloc sysfs attrs\n", __func__);
	return -ENOMEM;
}

static int __init lbt_sysfs_init(struct kobject *parent)
{
	int depth = get_topology_depth();
	int i;

	if (alloc_lbt_sysfs(depth + 1))
		goto out;

	for (i = 0; i <= depth; i++) {
		char buf[20];
		char *name;

		sprintf(buf, "overutil_ratio_level%d", i);
		name = kstrdup(buf, GFP_KERNEL);
		if (!name)
			goto out;

		lbt_attr_init(lbt_kattrs[i], name, 0644,
				show_overutil_ratio, store_overutil_ratio);
		lbt_attrs[i] = &lbt_kattrs[i].attr;
	}

	lbt_group.attrs = lbt_attrs;

	lbt_kobj = kobject_create_and_add("lbt", parent);
	if (!lbt_kobj)
		goto out;

	if (sysfs_create_group(lbt_kobj, &lbt_group))
		goto out;

	return 0;

out:
	free_lbt_sysfs();

	pr_err("LBT(%s): failed to create sysfs node\n", __func__);
	return -EINVAL;
}

/****************************************************************/
/*			Initialization				*/
/****************************************************************/
static void free_lbt_overutil(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (per_cpu(lbt_overutil, cpu))
			kfree(per_cpu(lbt_overutil, cpu));
	}
}

static int alloc_lbt_overutil(void)
{
	int cpu, depth = get_topology_depth();

	for_each_possible_cpu(cpu) {
		struct lbt_overutil *ou = kzalloc(sizeof(struct lbt_overutil) *
				(depth + 1), GFP_KERNEL);
		if (!ou)
			goto fail_alloc;

		per_cpu(lbt_overutil, cpu) = ou;
	}
	return 0;

fail_alloc:
	free_lbt_overutil();
	return -ENOMEM;
}

static int set_lbt_overutil(int level, const char *mask, int ratio)
{
	struct lbt_overutil *ou;
	struct cpumask cpus;
	bool top, overlap = false;
	int cpu;

	cpulist_parse(mask, &cpus);
	cpumask_and(&cpus, &cpus, cpu_possible_mask);
	if (!cpumask_weight(&cpus))
		return -ENODEV;

	/* If sibling cpus is same with possible cpus, it is top level */
	top = cpumask_equal(&cpus, cpu_possible_mask);

	/* If this level is overlapped with prev level, disable this level */
	if (level > 0) {
		ou = per_cpu(lbt_overutil, cpumask_first(&cpus));
		overlap = cpumask_equal(&cpus, &ou[level-1].cpus);
	}

	for_each_cpu(cpu, &cpus) {
		ou = per_cpu(lbt_overutil, cpu);
		cpumask_copy(&ou[level].cpus, &cpus);
		ou[level].ratio = overlap ? DISABLE_OU : ratio;
		ou[level].top = top;
	}

	return 0;
}

static int parse_lbt_overutil(struct device_node *dn)
{
	struct device_node *lbt, *ou;
	int level, depth = get_topology_depth();
	int ret = 0;

	lbt = of_get_child_by_name(dn, "lbt");
	if (!lbt)
		return -ENODEV;

	for (level = 0; level <= depth; level++) {
		char name[20];
		const char *mask[NR_CPUS];
		int ratio[NR_CPUS];
		int i, proplen;

		snprintf(name, sizeof(name), "overutil-level%d", level);
		ou = of_get_child_by_name(lbt, name);
		if (!ou) {
			ret = -ENODEV;
			goto out;
		}

		proplen = of_property_count_strings(ou, "cpus");
		if ((proplen < 0) || (proplen != of_property_count_u32_elems(ou, "ratio"))) {
			of_node_put(ou);
			ret = -ENODEV;
			goto out;
		}

		of_property_read_string_array(ou, "cpus", mask, proplen);
		of_property_read_u32_array(ou, "ratio", ratio, proplen);
		of_node_put(ou);

		for (i = 0; i < proplen; i++) {
			ret = set_lbt_overutil(level, mask[i], ratio[i]);
			if (ret)
				goto out;
		}
	}

out:
	of_node_put(lbt);
	return ret;
}

static int __init init_lbt(void)
{
	struct device_node *dn;
	int ret;

	dn = of_find_node_by_path("/cpus/ehmp");
	if (!dn)
		return 0;

	ret = alloc_lbt_overutil();
	if (ret) {
		pr_err("Failed to allocate lbt_overutil\n");
		goto out;
	}

	ret = parse_lbt_overutil(dn);
	if (ret) {
		pr_err("Failed to parse lbt_overutil\n");
		free_lbt_overutil();
		goto out;
	}

out:
	of_node_put(dn);
	return ret;
}
pure_initcall(init_lbt);
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

void gb_qos_update_request(struct gb_qos_request *req, u32 new_value)
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
#define cpu_selected(cpu)	(cpu >= 0)

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

	gb = global_boost();
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

/****************************************************************/
/*			On-time migration			*/
/****************************************************************/
#define TASK_TRACK_COUNT	5

#define ontime_task_cpu(p)		(ontime_of(p)->cpu)
#define ontime_flag(p)			(ontime_of(p)->flags)
#define ontime_migration_time(p)	(ontime_of(p)->avg.ontime_migration_time)
#define ontime_load_avg(p)		(ontime_of(p)->avg.load_avg)

#define cap_scale(v, s)		((v)*(s) >> SCHED_CAPACITY_SHIFT)
#define mincap_of(__cpu)	(sge_array[__cpu][SD_LEVEL0]->cap_states[0].cap)

/* Structure of ontime migration condition */
struct ontime_cond {
	unsigned long		up_threshold;
	unsigned long		down_threshold;
	unsigned int		min_residency_us;

	struct cpumask		src_cpus;
	struct cpumask		dst_cpus;

	struct ontime_cond	*next;
};
static struct ontime_cond *ontime_cond;

/* Structure of ontime migration environment */
struct ontime_env {
	struct rq		*dst_rq;
	int			dst_cpu;
	struct rq		*src_rq;
	int			src_cpu;
	struct task_struct	*target_task;
	int			boost_migration;
};
DEFINE_PER_CPU(struct ontime_env, ontime_env);

static unsigned long get_up_threshold(int cpu)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->src_cpus))
			return cond->up_threshold;

		cond = cond->next;
	}

	return -EINVAL;
}

static int set_up_threshold(int cpu, unsigned long val)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->src_cpus)) {
			cond->up_threshold = val;
			return 0;
		}

		cond = cond->next;
	}

	return -EINVAL;
}

static unsigned long get_down_threshold(int cpu)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->dst_cpus))
			return cond->down_threshold;

		cond = cond->next;
	}

	return -EINVAL;
}

static int set_down_threshold(int cpu, unsigned long val)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->dst_cpus)) {
			cond->down_threshold = val;
			return 0;
		}

		cond = cond->next;
	}

	return -EINVAL;
}

static unsigned int get_min_residency(int cpu)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->dst_cpus))
			return cond->min_residency_us;

		cond = cond->next;
	}

	return -EINVAL;
}

static int set_min_residency(int cpu, int val)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->dst_cpus)) {
			cond->min_residency_us = val;
			return 0;
		}

		cond = cond->next;
	}

	return -EINVAL;
}

static inline void include_ontime_task(struct task_struct *p, int dst_cpu)
{
	ontime_flag(p) = ONTIME;
	ontime_task_cpu(p) = dst_cpu;

	/* Manage time based on clock task of boot cpu(cpu0) */
	ontime_migration_time(p) = cpu_rq(0)->clock_task;
}

static inline void exclude_ontime_task(struct task_struct *p)
{
	ontime_task_cpu(p) = 0;
	ontime_migration_time(p) = 0;
	ontime_flag(p) = NOT_ONTIME;
}

static int
ontime_select_target_cpu(struct cpumask *dst_cpus, const struct cpumask *mask)
{
	int cpu;
	int dest_cpu = -1;
	unsigned int min_exit_latency = UINT_MAX;
	struct cpuidle_state *idle;

	rcu_read_lock();
	for_each_cpu_and(cpu, dst_cpus, mask) {
		if (!idle_cpu(cpu))
			continue;

		if (cpu_rq(cpu)->ontime_migrating)
			continue;

		idle = idle_get_state(cpu_rq(cpu));
		if (!idle) {
			rcu_read_unlock();
			return cpu;
		}

		if (idle && idle->exit_latency < min_exit_latency) {
			min_exit_latency = idle->exit_latency;
			dest_cpu = cpu;
		}
	}

	rcu_read_unlock();
	return dest_cpu;
}

extern struct sched_entity *__pick_next_entity(struct sched_entity *se);
static struct task_struct *
ontime_pick_heavy_task(struct sched_entity *se, struct cpumask *dst_cpus,
						int *boost_migration)
{
	struct task_struct *heaviest_task = NULL;
	struct task_struct *p;
	unsigned int max_util_avg = 0;
	int task_count = 0;
	int boosted = !!global_boost();

	/*
	 * Since current task does not exist in entity list of cfs_rq,
	 * check first that current task is heavy.
	 */
	p = task_of(se);
	if (boosted || ontime_load_avg(p) >= get_up_threshold(task_cpu(p))) {
		heaviest_task = task_of(se);
		max_util_avg = ontime_load_avg(task_of(se));
		if (boosted)
			*boost_migration = 1;
	}

	se = __pick_first_entity(se->cfs_rq);
	while (se && task_count < TASK_TRACK_COUNT) {
		/* Skip non-task entity */
		if (entity_is_cfs_rq(se))
			goto next_entity;

		p = task_of(se);
		if (schedtune_prefer_perf(p)) {
			heaviest_task = p;
			*boost_migration = 1;
			break;
		}

		if (!boosted && ontime_load_avg(p) <
				get_up_threshold(task_cpu(p)))
			goto next_entity;

		if (ontime_load_avg(p) > max_util_avg &&
		    cpumask_intersects(dst_cpus, tsk_cpus_allowed(p))) {
			heaviest_task = p;
			max_util_avg = ontime_load_avg(p);
			*boost_migration = boosted;
		}

next_entity:
		se = __pick_next_entity(se);
		task_count++;
	}

	return heaviest_task;
}

static int can_migrate(struct task_struct *p, struct ontime_env *env)
{
	if (!cpumask_test_cpu(env->dst_cpu, tsk_cpus_allowed(p)))
		return 0;

	if (task_running(env->src_rq, p))
		return 0;

	return 1;
}

static void move_task(struct task_struct *p, struct ontime_env *env)
{
	p->on_rq = TASK_ON_RQ_MIGRATING;
	deactivate_task(env->src_rq, p, 0);
	set_task_cpu(p, env->dst_cpu);

	activate_task(env->dst_rq, p, 0);
	p->on_rq = TASK_ON_RQ_QUEUED;
	check_preempt_curr(env->dst_rq, p, 0);
}

static int move_specific_task(struct task_struct *target, struct ontime_env *env)
{
	struct task_struct *p, *n;

	list_for_each_entry_safe(p, n, &env->src_rq->cfs_tasks, se.group_node) {
		if (!can_migrate(p, env))
			continue;

		if (p != target)
			continue;

		move_task(p, env);
		return 1;
	}

	return 0;
}

static int ontime_migration_cpu_stop(void *data)
{
	struct ontime_env *env = data;
	struct rq *src_rq, *dst_rq;
	int src_cpu, dst_cpu;
	struct task_struct *p;
	struct sched_domain *sd;
	int boost_migration;

	/* Initialize environment data */
	src_rq = env->src_rq;
	dst_rq = env->dst_rq = cpu_rq(env->dst_cpu);
	src_cpu = env->src_cpu = env->src_rq->cpu;
	dst_cpu = env->dst_cpu;
	p = env->target_task;
	boost_migration = env->boost_migration;

	raw_spin_lock_irq(&src_rq->lock);

	if (!(ontime_flag(p) & ONTIME_MIGRATING))
		goto out_unlock;

	if (p->exit_state)
		goto out_unlock;

	if (unlikely(src_cpu != smp_processor_id()))
		goto out_unlock;

	if (src_rq->nr_running <= 1)
		goto out_unlock;

	if (src_rq != task_rq(p))
		goto out_unlock;

	BUG_ON(src_rq == dst_rq);

	double_lock_balance(src_rq, dst_rq);

	rcu_read_lock();
	for_each_domain(dst_cpu, sd)
		if (cpumask_test_cpu(src_cpu, sched_domain_span(sd)))
			break;

	if (likely(sd) && move_specific_task(p, env)) {
		if (boost_migration) {
			/* boost task is not classified as ontime task */
			exclude_ontime_task(p);
		} else {
			include_ontime_task(p, dst_cpu);
		}

		rcu_read_unlock();
		double_unlock_balance(src_rq, dst_rq);

		trace_ehmp_ontime_migration(p, ontime_of(p)->avg.load_avg,
					src_cpu, dst_cpu, boost_migration);
		goto success_unlock;
	}

	rcu_read_unlock();
	double_unlock_balance(src_rq, dst_rq);

out_unlock:
	exclude_ontime_task(p);

success_unlock:
	src_rq->active_balance = 0;
	dst_rq->ontime_migrating = 0;

	raw_spin_unlock_irq(&src_rq->lock);
	put_task_struct(p);

	return 0;
}

int ontime_task_wakeup(struct task_struct *p)
{
	struct ontime_cond *cond;
	struct cpumask target_mask;
	u64 delta;
	int target_cpu = -1;

	/* When wakeup task is on ontime migrating, do not ontime wakeup */
	if (ontime_flag(p) == ONTIME_MIGRATING)
		return -1;

	/*
	 * When wakeup task satisfies ontime condition to up migration,
	 * check there is a possible target cpu.
	 */
	if (ontime_load_avg(p) >= get_up_threshold(task_cpu(p))) {
		cpumask_clear(&target_mask);

		for (cond = ontime_cond; cond != NULL; cond = cond->next)
			if (cpumask_test_cpu(task_cpu(p), &cond->src_cpus)) {
				cpumask_copy(&target_mask, &cond->dst_cpus);
				break;
			}

		target_cpu = ontime_select_target_cpu(&target_mask, tsk_cpus_allowed(p));

		if (cpu_selected(target_cpu)) {
			trace_ehmp_ontime_task_wakeup(p, task_cpu(p),
					target_cpu, "up ontime");
			goto ontime_up;
		}
	}

	/*
	 * If wakeup task is not ontime and doesn't satisfy ontime condition,
	 * it cannot be ontime task.
	 */
	if (ontime_flag(p) == NOT_ONTIME)
		goto ontime_out;

	if (ontime_flag(p) == ONTIME) {
		/*
		 * If wakeup task is ontime but doesn't keep ontime condition,
		 * exclude this task from ontime.
		 */
		delta = cpu_rq(0)->clock_task - ontime_migration_time(p);
		delta = delta >> 10;

		if (delta > get_min_residency(ontime_task_cpu(p)) &&
				ontime_load_avg(p) < get_down_threshold(ontime_task_cpu(p))) {
			trace_ehmp_ontime_task_wakeup(p, task_cpu(p), -1,
					"release ontime");
			goto ontime_out;
		}

		/*
		 * If there is a possible cpu to stay ontime, task will wake up at this cpu.
		 */
		cpumask_copy(&target_mask, cpu_coregroup_mask(ontime_task_cpu(p)));
		target_cpu = ontime_select_target_cpu(&target_mask, tsk_cpus_allowed(p));

		if (cpu_selected(target_cpu)) {
			trace_ehmp_ontime_task_wakeup(p, task_cpu(p),
					target_cpu, "stay ontime");
			goto ontime_stay;
		}

		trace_ehmp_ontime_task_wakeup(p, task_cpu(p), -1, "banished");
		goto ontime_out;
	}

	if (!cpu_selected(target_cpu))
		goto ontime_out;

ontime_up:
	include_ontime_task(p, target_cpu);

ontime_stay:
	return target_cpu;

ontime_out:
	exclude_ontime_task(p);
	return -1;
}

static void ontime_update_next_balance(int cpu, struct ontime_avg *oa)
{
	if (cpumask_test_cpu(cpu, cpu_coregroup_mask(maxcap_cpu)))
		return;

	if (oa->load_avg < get_up_threshold(cpu))
		return;

	/*
	 * Update the next_balance of this cpu because tick is most likely
	 * to occur first in currently running cpu.
	 */
	cpu_rq(smp_processor_id())->next_balance = jiffies;
}

extern u64 decay_load(u64 val, u64 n);
static u32 __accumulate_pelt_segments(u64 periods, u32 d1, u32 d3)
{
	u32 c1, c2, c3 = d3;

	c1 = decay_load((u64)d1, periods);
	c2 = LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX, periods) - 1024;

	return c1 + c2 + c3;
}

/****************************************************************/
/*			External APIs				*/
/****************************************************************/
void ontime_trace_task_info(struct task_struct *p)
{
	trace_ehmp_ontime_load_avg_task(p, &ontime_of(p)->avg, ontime_flag(p));
}

DEFINE_PER_CPU(struct cpu_stop_work, ontime_migration_work);
static DEFINE_SPINLOCK(om_lock);

void ontime_migration(void)
{
	struct ontime_cond *cond;
	int cpu;

	if (!spin_trylock(&om_lock))
		return;

	for (cond = ontime_cond; cond != NULL; cond = cond->next) {
		for_each_cpu_and(cpu, &cond->src_cpus, cpu_active_mask) {
			unsigned long flags;
			struct rq *rq;
			struct sched_entity *se;
			struct task_struct *p;
			int dst_cpu;
			struct ontime_env *env = &per_cpu(ontime_env, cpu);
			int boost_migration = 0;

			rq = cpu_rq(cpu);
			raw_spin_lock_irqsave(&rq->lock, flags);

			/*
			 * Ontime migration is not performed when active balance
			 * is in progress.
			 */
			if (rq->active_balance) {
				raw_spin_unlock_irqrestore(&rq->lock, flags);
				continue;
			}

			/*
			 * No need to migration if source cpu does not have cfs
			 * tasks.
			 */
			if (!rq->cfs.curr) {
				raw_spin_unlock_irqrestore(&rq->lock, flags);
				continue;
			}

			se = rq->cfs.curr;

			/* Find task entity if entity is cfs_rq. */
			if (entity_is_cfs_rq(se)) {
				struct cfs_rq *cfs_rq;

				cfs_rq = se->my_q;
				while (cfs_rq) {
					se = cfs_rq->curr;
					cfs_rq = se->my_q;
				}
			}

			/*
			 * Select cpu to migrate the task to. Return negative number
			 * if there is no idle cpu in sg.
			 */
			dst_cpu = ontime_select_target_cpu(&cond->dst_cpus, cpu_active_mask);
			if (dst_cpu < 0) {
				raw_spin_unlock_irqrestore(&rq->lock, flags);
				continue;
			}

			/*
			 * Pick task to be migrated. Return NULL if there is no
			 * heavy task in rq.
			 */
			p = ontime_pick_heavy_task(se, &cond->dst_cpus,
							&boost_migration);
			if (!p) {
				raw_spin_unlock_irqrestore(&rq->lock, flags);
				continue;
			}

			ontime_flag(p) = ONTIME_MIGRATING;
			get_task_struct(p);

			/* Set environment data */
			env->dst_cpu = dst_cpu;
			env->src_rq = rq;
			env->target_task = p;
			env->boost_migration = boost_migration;

			/* Prevent active balance to use stopper for migration */
			rq->active_balance = 1;

			cpu_rq(dst_cpu)->ontime_migrating = 1;

			raw_spin_unlock_irqrestore(&rq->lock, flags);

			/* Migrate task through stopper */
			stop_one_cpu_nowait(cpu,
				ontime_migration_cpu_stop, env,
				&per_cpu(ontime_migration_work, cpu));
		}
	}

	spin_unlock(&om_lock);
}

int ontime_can_migration(struct task_struct *p, int dst_cpu)
{
	u64 delta;

	if (ontime_flag(p) & NOT_ONTIME) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, true, "not ontime");
		return true;
	}

	if (ontime_flag(p) & ONTIME_MIGRATING) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, false, "migrating");
		return false;
	}

	if (cpumask_test_cpu(dst_cpu, cpu_coregroup_mask(ontime_task_cpu(p)))) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, true, "same coregroup");
		return true;
	}

	if (capacity_orig_of(dst_cpu) > capacity_orig_of(ontime_task_cpu(p))) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, true, "bigger cpu");
		return true;
	}

	/*
	 * At this point, task is "ontime task" and running on big
	 * and load balancer is trying to migrate task to LITTLE.
	 */
	delta = cpu_rq(0)->clock_task - ontime_migration_time(p);
	delta = delta >> 10;
	if (delta <= get_min_residency(ontime_task_cpu(p))) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, false, "min residency");
		return false;
	}

	if (cpu_rq(task_cpu(p))->nr_running > 1) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, true, "big is busy");
		goto release;
	}

	if (ontime_load_avg(p) >= get_down_threshold(ontime_task_cpu(p))) {
		trace_ehmp_ontime_check_migrate(p, dst_cpu, false, "heavy task");
		return false;
	}

	trace_ehmp_ontime_check_migrate(p, dst_cpu, true, "ontime_release");
release:
	exclude_ontime_task(p);

	return true;
}

/*
 * ontime_update_load_avg : load tracking for ontime-migration
 *
 * @sa : sched_avg to be updated
 * @delta : elapsed time since last update
 * @period_contrib : amount already accumulated against our next period
 * @scale_freq : scale vector of cpu frequency
 * @scale_cpu : scale vector of cpu capacity
 */
void ontime_update_load_avg(u64 delta, int cpu, unsigned long weight, struct sched_avg *sa)
{
	struct ontime_avg *oa = &se_of(sa)->ontime.avg;
	unsigned long scale_freq, scale_cpu;
	u32 contrib = (u32)delta; /* p == 0 -> delta < 1024 */
	u64 periods;

	scale_freq = arch_scale_freq_capacity(NULL, cpu);
	scale_cpu = arch_scale_cpu_capacity(NULL, cpu);

	delta += oa->period_contrib;
	periods = delta / 1024; /* A period is 1024us (~1ms) */

	if (periods) {
		oa->load_sum = decay_load(oa->load_sum, periods);

		delta %= 1024;
		contrib = __accumulate_pelt_segments(periods,
				1024 - oa->period_contrib, delta);
	}
	oa->period_contrib = delta;

	if (weight) {
		contrib = cap_scale(contrib, scale_freq);
		oa->load_sum += contrib * scale_cpu;
	}

	if (!periods)
		return;

	oa->load_avg = div_u64(oa->load_sum, LOAD_AVG_MAX - 1024 + oa->period_contrib);
	ontime_update_next_balance(cpu, oa);
}

void ontime_new_entity_load(struct task_struct *parent, struct sched_entity *se)
{
	struct ontime_entity *ontime;

	if (entity_is_cfs_rq(se))
		return;

	ontime = &se->ontime;

	ontime->avg.load_sum = ontime_of(parent)->avg.load_sum;
	ontime->avg.load_avg = ontime_of(parent)->avg.load_avg;
	ontime->avg.ontime_migration_time = 0;
	ontime->avg.period_contrib = 1023;
	ontime->flags = NOT_ONTIME;

	trace_ehmp_ontime_new_entity_load(task_of(se), &ontime->avg);
}

/****************************************************************/
/*				SYSFS				*/
/****************************************************************/
static ssize_t show_up_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ontime_cond *cond = ontime_cond;
	int ret = 0;

	while (cond) {
		ret += sprintf(buf + ret, "cpu%*pbl: %ld\n",
				cpumask_pr_args(&cond->src_cpus),
				cond->up_threshold);

		cond = cond->next;
	}

	return ret;
}

static ssize_t store_up_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	unsigned long val;
	int cpu;

	if (sscanf(buf, "%d %lu", &cpu, &val) != 2)
		return -EINVAL;

	if (!cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;

	val = val > 1024 ? 1024 : val;

	if (set_up_threshold(cpu, val))
		return -EINVAL;

	return count;
}

static struct kobj_attribute up_threshold_attr =
__ATTR(up_threshold, 0644, show_up_threshold, store_up_threshold);

static ssize_t show_down_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ontime_cond *cond = ontime_cond;
	int ret = 0;

	while (cond) {
		ret += sprintf(buf + ret, "cpu%*pbl: %ld\n",
				cpumask_pr_args(&cond->dst_cpus),
				cond->down_threshold);

		cond = cond->next;
	}

	return ret;
}

static ssize_t store_down_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	unsigned long val;
	int cpu;

	if (sscanf(buf, "%d %lu", &cpu, &val) != 2)
		return -EINVAL;

	if (!cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;

	val = val > 1024 ? 1024 : val;

	if (set_down_threshold(cpu, val))
		return -EINVAL;

	return count;
}

static struct kobj_attribute down_threshold_attr =
__ATTR(down_threshold, 0644, show_down_threshold, store_down_threshold);

static ssize_t show_min_residency(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ontime_cond *cond = ontime_cond;
	int ret = 0;

	while (cond) {
		ret += sprintf(buf + ret, "cpu%*pbl: %d\n",
				cpumask_pr_args(&cond->dst_cpus),
				cond->min_residency_us);

		cond = cond->next;
	}

	return ret;
}

static ssize_t store_min_residency(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	int val;
	int cpu;

	if (sscanf(buf, "%d %d", &cpu, &val) != 2)
		return -EINVAL;

	if (!cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;

	val = val < 0 ? 0 : val;

	if (set_min_residency(cpu, val))
		return -EINVAL;

	return count;
}

static struct kobj_attribute min_residency_attr =
__ATTR(min_residency, 0644, show_min_residency, store_min_residency);

static struct attribute *ontime_attrs[] = {
	&min_residency_attr.attr,
	&up_threshold_attr.attr,
	&down_threshold_attr.attr,
	NULL,
};

static const struct attribute_group ontime_group = {
	.attrs = ontime_attrs,
};

static struct kobject *ontime_kobj;

static int __init ontime_sysfs_init(struct kobject *parent)
{
	int ret;

	ontime_kobj = kobject_create_and_add("ontime", parent);
	ret = sysfs_create_group(ontime_kobj, &ontime_group);

	return ret;
}

/****************************************************************/
/*			initialization				*/
/****************************************************************/
static void
parse_ontime(struct device_node *dn, struct ontime_cond *cond, int step)
{
	struct device_node *ontime, *on_step;
	char name[10];
	int prop;

	/*
	 * Initilize default values:
	 *   up_threshold	= 40% of Source CPU's maximum capacity
	 *   down_threshold	= 50% of Destination CPU's minimum capacity
	 *   min_residency	= 8ms
	 */
	cond->up_threshold =
		capacity_orig_of(cpumask_first(&cond->src_cpus)) * 40 / 100;
	cond->down_threshold =
		mincap_of(cpumask_first(&cond->dst_cpus)) * 50 / 100;
	cond->min_residency_us = 8192;

	ontime = of_get_child_by_name(dn, "ontime");
	if (!ontime)
		return;

	snprintf(name, sizeof(name), "step%d", step);
	on_step = of_get_child_by_name(ontime, name);
	if (!on_step)
		return;

	of_property_read_u32(on_step, "up-threshold", &prop);
	cond->up_threshold = prop;

	of_property_read_u32(on_step, "down-threshold", &prop);
	cond->down_threshold = prop;

	of_property_read_u32(on_step, "min-residency-us", &prop);
	cond->min_residency_us = prop;
}

static int __init init_ontime(void)
{
	struct cpumask prev_cpus;
	struct ontime_cond *cond, *last;
	struct device_node *dn;
	int cpu, step = 0;

	dn = of_find_node_by_path("/cpus/ehmp");
	if (!dn)
		return 0;

	cpumask_clear(&prev_cpus);

	for_each_possible_cpu(cpu) {
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		if (cpumask_empty(&prev_cpus)) {
			cpumask_copy(&prev_cpus, cpu_coregroup_mask(cpu));
			continue;
		}

		cond = kzalloc(sizeof(struct ontime_cond), GFP_KERNEL);

		cpumask_copy(&cond->dst_cpus, cpu_coregroup_mask(cpu));
		cpumask_copy(&cond->src_cpus, &prev_cpus);

		parse_ontime(dn, cond, step++);

		cpumask_copy(&prev_cpus, cpu_coregroup_mask(cpu));

		/* Add linked list of ontime_cond at last */
		cond->next = NULL;
		if (ontime_cond)
			last->next = cond;
		else
			ontime_cond = cond;
		last = cond;
	}

	of_node_put(dn);
	return 0;
}
pure_initcall(init_ontime);

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

	ret = ontime_sysfs_init(ehmp_kobj);
	if (ret)
		return ret;

	ret = lbt_sysfs_init(ehmp_kobj);
	if (ret)
		return ret;

	return 0;
}
late_initcall(init_sysfs);
