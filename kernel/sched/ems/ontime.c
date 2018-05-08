/*
 * On-time Migration Feature for Exynos Mobile Scheduler (EMS)
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * LEE DAEYEONG <daeyeong.lee@samsung.com>
 */

#include <linux/sched.h>
#include <linux/cpuidle.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/ems.h>
#include <linux/sched_energy.h>

#include <trace/events/ems.h>

#include "../sched.h"
#include "../tune.h"
#include "./ems.h"

/****************************************************************/
/*			On-time migration			*/
/****************************************************************/
#define TASK_TRACK_COUNT	5
#define MAX_CAPACITY_CPU	(NR_CPUS - 1)

#define ontime_task_cpu(p)		(ontime_of(p)->cpu)
#define ontime_flag(p)			(ontime_of(p)->flags)
#define ontime_load_avg(p)		(ontime_of(p)->avg.load_avg)

#define cap_scale(v, s)		((v)*(s) >> SCHED_CAPACITY_SHIFT)

#define entity_is_cfs_rq(se)	(se->my_q)
#define entity_is_task(se)	(!se->my_q)

/* Structure of ontime migration condition */
struct ontime_cond {
	bool			enabled;

	unsigned long		up_threshold;
	unsigned long		down_threshold;

	int			coregroup;
	struct cpumask		cpus;

	struct list_head	list;

	/* kobject for sysfs group */
	struct kobject		kobj;
};
LIST_HEAD(cond_list);

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
	struct ontime_cond *curr;

	list_for_each_entry(curr, &cond_list, list) {
		if (cpumask_test_cpu(cpu, &curr->cpus))
			return curr->up_threshold;
	}

	return ULONG_MAX;
}

static unsigned long get_down_threshold(int cpu)
{
	struct ontime_cond *curr;

	list_for_each_entry(curr, &cond_list, list) {
		if (cpumask_test_cpu(cpu, &curr->cpus))
			return curr->down_threshold;
	}

	return 0;
}

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static inline struct sched_entity *se_of(struct sched_avg *sa)
{
	return container_of(sa, struct sched_entity, avg);
}

static inline void include_ontime_task(struct task_struct *p, int dst_cpu)
{
	ontime_flag(p) = ONTIME;
	ontime_task_cpu(p) = dst_cpu;
}

static inline void exclude_ontime_task(struct task_struct *p)
{
	ontime_task_cpu(p) = 0;
	ontime_flag(p) = NOT_ONTIME;
}

static int
ontime_select_target_cpu(struct cpumask *dst_cpus, const struct cpumask *mask)
{
	int cpu, dest_cpu = -1;
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
	int boosted = !!global_boosted() || !!schedtune_prefer_perf(task_of(se));

	/*
	 * Since current task does not exist in entity list of cfs_rq,
	 * check first that current task is heavy.
	 */
	p = task_of(se);
	if (boosted) {
		*boost_migration = 1;
		return p;
	}
	if (ontime_load_avg(p) >= get_up_threshold(task_cpu(p))) {
		heaviest_task = p;
		max_util_avg = ontime_load_avg(p);
		*boost_migration = 0;
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

		if (ontime_load_avg(p) < get_up_threshold(task_cpu(p)))
			goto next_entity;

		if (ontime_load_avg(p) > max_util_avg &&
		    cpumask_intersects(dst_cpus, tsk_cpus_allowed(p))) {
			heaviest_task = p;
			max_util_avg = ontime_load_avg(p);
			*boost_migration = 0;
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
	struct task_struct *p;
	struct sched_domain *sd;
	int src_cpu, dst_cpu;
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

		trace_ems_ontime_migration(p, ontime_of(p)->avg.load_avg,
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

static void ontime_update_next_balance(int cpu, struct ontime_avg *oa)
{
	if (cpumask_test_cpu(cpu, cpu_coregroup_mask(MAX_CAPACITY_CPU)))
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
	trace_ems_ontime_load_avg_task(p, &ontime_of(p)->avg, ontime_flag(p));
}

DEFINE_PER_CPU(struct cpu_stop_work, ontime_migration_work);
static DEFINE_SPINLOCK(om_lock);

void ontime_migration(void)
{
	struct ontime_cond *curr, *next = NULL;
	int cpu;

	if (!spin_trylock(&om_lock))
		return;

	list_for_each_entry(curr, &cond_list, list) {
		next = list_next_entry(curr, list);
		if (!next)
			break;

		for_each_cpu_and(cpu, &curr->cpus, cpu_active_mask) {
			unsigned long flags;
			struct rq *rq = cpu_rq(cpu);
			struct sched_entity *se;
			struct task_struct *p;
			struct ontime_env *env = &per_cpu(ontime_env, cpu);
			int dst_cpu;
			int boost_migration = 0;

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

			/* Find task entity if entity is cfs_rq. */
			se = rq->cfs.curr;
			if (entity_is_cfs_rq(se)) {
				struct cfs_rq *cfs_rq = se->my_q;

				while (cfs_rq) {
					se = cfs_rq->curr;
					cfs_rq = se->my_q;
				}
			}

			/*
			 * Select cpu to migrate the task to. Return negative number
			 * if there is no idle cpu in sg.
			 */
			dst_cpu = ontime_select_target_cpu(&next->cpus, cpu_active_mask);
			if (dst_cpu < 0) {
				raw_spin_unlock_irqrestore(&rq->lock, flags);
				continue;
			}

			/*
			 * Pick task to be migrated. Return NULL if there is no
			 * heavy task in rq.
			 */
			p = ontime_pick_heavy_task(se, &next->cpus, &boost_migration);
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
			stop_one_cpu_nowait(cpu, ontime_migration_cpu_stop, env,
				&per_cpu(ontime_migration_work, cpu));
		}
	}

	spin_unlock(&om_lock);
}

int ontime_task_wakeup(struct task_struct *p)
{
	struct ontime_cond *curr, *next = NULL;
	struct cpumask target_mask;
	int src_cpu = task_cpu(p);
	int dst_cpu = -1;

	/* When wakeup task is on ontime migrating, do not ontime wakeup */
	if (ontime_flag(p) == ONTIME_MIGRATING)
		return -1;

	/*
	 * When wakeup task satisfies ontime condition to up migration,
	 * check there is a possible target cpu.
	 */
	if (ontime_load_avg(p) >= get_up_threshold(src_cpu)) {
		list_for_each_entry (curr, &cond_list, list) {
			next = list_next_entry(curr, list);
			if (cpumask_test_cpu(src_cpu, &curr->cpus)) {
				cpumask_copy(&target_mask, &next->cpus);
				break;
			}
		}

		dst_cpu = ontime_select_target_cpu(&target_mask, tsk_cpus_allowed(p));

		if (cpu_selected(dst_cpu)) {
			trace_ems_ontime_task_wakeup(p, src_cpu, dst_cpu, "up ontime");
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
		if (ontime_load_avg(p) < get_down_threshold(ontime_task_cpu(p))) {
			trace_ems_ontime_task_wakeup(p, src_cpu, -1, "release ontime");
			goto ontime_out;
		}

		/*
		 * If there is a possible cpu to stay ontime, task will wake up at this cpu.
		 */
		cpumask_copy(&target_mask, cpu_coregroup_mask(ontime_task_cpu(p)));
		dst_cpu = ontime_select_target_cpu(&target_mask, tsk_cpus_allowed(p));

		if (cpu_selected(dst_cpu)) {
			trace_ems_ontime_task_wakeup(p, src_cpu, dst_cpu, "stay ontime");
			goto ontime_stay;
		}

		trace_ems_ontime_task_wakeup(p, src_cpu, -1, "banished");
		goto ontime_out;
	}

	if (!cpu_selected(dst_cpu))
		goto ontime_out;

ontime_up:
	include_ontime_task(p, dst_cpu);

ontime_stay:
	return dst_cpu;

ontime_out:
	exclude_ontime_task(p);
	return -1;
}

int ontime_can_migration(struct task_struct *p, int dst_cpu)
{
	if (ontime_flag(p) & NOT_ONTIME) {
		trace_ems_ontime_check_migrate(p, dst_cpu, true, "not ontime");
		return true;
	}

	if (ontime_flag(p) & ONTIME_MIGRATING) {
		trace_ems_ontime_check_migrate(p, dst_cpu, false, "migrating");
		return false;
	}

	if (cpumask_test_cpu(dst_cpu, cpu_coregroup_mask(ontime_task_cpu(p)))) {
		trace_ems_ontime_check_migrate(p, dst_cpu, true, "same coregroup");
		return true;
	}

	if (capacity_orig_of(dst_cpu) > capacity_orig_of(ontime_task_cpu(p))) {
		trace_ems_ontime_check_migrate(p, dst_cpu, true, "bigger cpu");
		return true;
	}

	/*
	 * At this point, task is "ontime task" and running on big
	 * and load balancer is trying to migrate task to LITTLE.
	 */
	if (cpu_rq(task_cpu(p))->nr_running > 1) {
		trace_ems_ontime_check_migrate(p, dst_cpu, true, "big is busy");
		goto release;
	}

	if (ontime_load_avg(p) >= get_down_threshold(ontime_task_cpu(p))) {
		trace_ems_ontime_check_migrate(p, dst_cpu, false, "heavy task");
		return false;
	}

	trace_ems_ontime_check_migrate(p, dst_cpu, true, "ontime_release");
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
	ontime->avg.period_contrib = 1023;
	ontime->flags = NOT_ONTIME;

	trace_ems_ontime_new_entity_load(task_of(se), &ontime->avg);
}

/****************************************************************/
/*				SYSFS				*/
/****************************************************************/
static struct kobject *ontime_kobj;

struct ontime_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *, char *);
	ssize_t (*store)(struct kobject *, const char *, size_t count);
};

#define ontime_attr_rw(_name)				\
static struct ontime_attr _name##_attr =		\
__ATTR(_name, 0644, show_##_name, store_##_name)

#define ontime_show(_name)							\
static ssize_t show_##_name(struct kobject *k, char *buf)			\
{										\
	struct ontime_cond *cond = container_of(k, struct ontime_cond, kobj);	\
										\
	return sprintf(buf, "%u\n", (unsigned int)cond->_name);			\
}

#define ontime_store(_name, _type, _max)					\
static ssize_t store_##_name(struct kobject *k, const char *buf, size_t count)	\
{										\
	unsigned int val;							\
	struct ontime_cond *cond = container_of(k, struct ontime_cond, kobj);	\
										\
	if (!sscanf(buf, "%u", &val))						\
		return -EINVAL;							\
										\
	val = val > _max ? _max : val;						\
	cond->_name = (_type)val;						\
										\
	return count;								\
}

ontime_show(up_threshold);
ontime_show(down_threshold);
ontime_store(up_threshold, unsigned long, 1024);
ontime_store(down_threshold, unsigned long, 1024);
ontime_attr_rw(up_threshold);
ontime_attr_rw(down_threshold);

static ssize_t show(struct kobject *kobj, struct attribute *at, char *buf)
{
	struct ontime_attr *oattr = container_of(at, struct ontime_attr, attr);

	return oattr->show(kobj, buf);
}

static ssize_t store(struct kobject *kobj, struct attribute *at,
		     const char *buf, size_t count)
{
	struct ontime_attr *oattr = container_of(at, struct ontime_attr, attr);

	return oattr->store(kobj, buf, count);
}

static const struct sysfs_ops ontime_sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct attribute *ontime_attrs[] = {
	&up_threshold_attr.attr,
	&down_threshold_attr.attr,
	NULL
};

static struct kobj_type ktype_ontime = {
	.sysfs_ops	= &ontime_sysfs_ops,
	.default_attrs	= ontime_attrs,
};

static int __init ontime_sysfs_init(void)
{
	struct ontime_cond *curr;

	if (list_empty(&cond_list))
		return 0;

	ontime_kobj = kobject_create_and_add("ontime", ems_kobj);
	if (!ontime_kobj)
		goto out;

	/* Add ontime sysfs node for each coregroup */
	list_for_each_entry(curr, &cond_list, list) {
		int ret;

		/* If ontime is disabled in this coregroup, do not create sysfs node */
		if (!curr->enabled)
			continue;

		ret = kobject_init_and_add(&curr->kobj, &ktype_ontime,
				ontime_kobj, "coregroup%d", curr->coregroup);
		if (ret)
			goto out;
	}

	return 0;

out:
	pr_err("ONTIME(%s): failed to create sysfs node\n", __func__);
	return -EINVAL;
}
late_initcall(ontime_sysfs_init);

/****************************************************************/
/*			initialization				*/
/****************************************************************/
static void
parse_ontime(struct device_node *dn, struct ontime_cond *cond, int cnt)
{
	struct device_node *ontime, *coregroup;
	char name[15];
	unsigned int prop;
	int res = 0;

	ontime = of_get_child_by_name(dn, "ontime");
	if (!ontime)
		goto disable;

	snprintf(name, sizeof(name), "coregroup%d", cnt);
	coregroup = of_get_child_by_name(ontime, name);
	if (!coregroup)
		goto disable;
	cond->coregroup = cnt;

	/* If any of ontime parameter isn't, disable ontime of this coregroup */
	res |= of_property_read_u32(coregroup, "up-threshold", &prop);
	cond->up_threshold = prop;

	res |= of_property_read_u32(coregroup, "down-threshold", &prop);
	cond->down_threshold = prop;

	if (res)
		goto disable;

	cond->enabled = true;
	return;

disable:
	cond->enabled = false;
	cond->up_threshold = ULONG_MAX;
	cond->down_threshold = 0;
}

static int __init init_ontime(void)
{
	struct ontime_cond *cond;
	struct device_node *dn;
	int cpu, cnt = 0;

	dn = of_find_node_by_path("/cpus/ems");
	if (!dn)
		return 0;

	INIT_LIST_HEAD(&cond_list);

	for_each_possible_cpu(cpu) {
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		cond = kzalloc(sizeof(struct ontime_cond), GFP_KERNEL);

		cpumask_copy(&cond->cpus, cpu_coregroup_mask(cpu));

		parse_ontime(dn, cond, cnt++);

		list_add_tail(&cond->list, &cond_list);
	}

	of_node_put(dn);
	return 0;
}
pure_initcall(init_ontime);
