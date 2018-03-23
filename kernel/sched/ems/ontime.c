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
#define MAX_CAPACITY_CPU	7

#define ontime_task_cpu(p)		(ontime_of(p)->cpu)
#define ontime_flag(p)			(ontime_of(p)->flags)
#define ontime_migration_time(p)	(ontime_of(p)->avg.ontime_migration_time)
#define ontime_load_avg(p)		(ontime_of(p)->avg.load_avg)

#define cap_scale(v, s)		((v)*(s) >> SCHED_CAPACITY_SHIFT)
#define mincap_of(__cpu)	(sge_array[__cpu][SD_LEVEL0]->cap_states[0].cap)

#define entity_is_cfs_rq(se)	(se->my_q)
#define entity_is_task(se)	(!se->my_q)

/* Structure of ontime migration condition */
struct ontime_cond {
	unsigned long		up_threshold;
	unsigned long		down_threshold;
	unsigned int		min_residency;

	struct cpumask		src_cpus;
	struct cpumask		dst_cpus;

	struct ontime_cond	*next;

	/* kobject attrbute for sysfs */
	struct kobj_attribute	up_threshold_attr;
	struct kobj_attribute	down_threshold_attr;
	struct kobj_attribute	min_residency_attr;
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

static unsigned int get_min_residency(int cpu)
{
	struct ontime_cond *cond = ontime_cond;

	while (cond) {
		if (cpumask_test_cpu(cpu, &cond->dst_cpus))
			return cond->min_residency;

		cond = cond->next;
	}

	return -EINVAL;
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
#define NUM_OF_ONTIME_NODE	3

#define ontime_attr_init(_attr, _name, _mode, _show, _store)		\
	sysfs_attr_init(&_attr.attr);					\
	_attr.attr.name = _name;					\
	_attr.attr.mode = VERIFY_OCTAL_PERMISSIONS(_mode);		\
	_attr.show	= _show;					\
	_attr.store	= _store;

#define ontime_show(_name)						\
static ssize_t show_##_name(struct kobject *kobj,			\
		struct kobj_attribute *attr, char *buf)			\
{									\
	struct ontime_cond *cond = container_of(attr,			\
			struct ontime_cond, _name##_attr);		\
									\
	return sprintf(buf, "%u\n", (unsigned int)cond->_name);		\
}

#define ontime_store(_name, _type, _max)				\
static ssize_t store_##_name(struct kobject *kobj,			\
		struct kobj_attribute *attr, const char *buf,		\
		size_t count)						\
{									\
	unsigned int val;						\
	struct ontime_cond *cond = container_of(attr,			\
			struct ontime_cond, _name##_attr);		\
									\
	if (!sscanf(buf, "%u", &val))					\
		return -EINVAL;						\
									\
	val = val > _max ? _max : val;					\
	cond->_name = (_type)val;					\
									\
	return count;							\
}

static struct kobject *ontime_kobj;
static struct attribute_group ontime_group;
static struct attribute **ontime_attrs;

ontime_show(up_threshold);
ontime_show(down_threshold);
ontime_show(min_residency);
ontime_store(up_threshold, unsigned long, 1024);
ontime_store(down_threshold, unsigned long, 1024);
ontime_store(min_residency, unsigned int, UINT_MAX);

static int alloc_ontime_sysfs(int size)
{
	ontime_attrs = kzalloc(sizeof(struct attribute *) * (size + 1),
			GFP_KERNEL);
	if (!ontime_attrs)
		goto fail_alloc;

	return 0;

fail_alloc:
	pr_err("ONTIME(%s): failed to alloc sysfs attrs\n", __func__);
	return -ENOMEM;
}

int __init ontime_sysfs_init(struct kobject *parent)
{
	struct ontime_cond *cond = ontime_cond;
	int count, step, i;

	count = 0;
	while (cond) {
		count++;
		cond = cond->next;
	}

	alloc_ontime_sysfs(count * NUM_OF_ONTIME_NODE);

	i = 0;
	step = 0;
	cond = ontime_cond;
	while (cond) {
		char buf[20];
		char *name;

		/* Init up_threshold node */
		sprintf(buf, "up_threshold_step%d", step);
		name = kstrdup(buf, GFP_KERNEL);
		if (!name)
			goto out;

		ontime_attr_init(cond->up_threshold_attr, name, 0644,
				show_up_threshold, store_up_threshold);
		ontime_attrs[i++] = &cond->up_threshold_attr.attr;

		/* Init down_threshold node */
		sprintf(buf, "down_threshold_step%d", step);
		name = kstrdup(buf, GFP_KERNEL);
		if (!name)
			goto out;

		ontime_attr_init(cond->down_threshold_attr, name, 0644,
				show_down_threshold, store_down_threshold);
		ontime_attrs[i++] = &cond->down_threshold_attr.attr;

		/* Init min_residency node */
		sprintf(buf, "min_residency_step%d", step);
		name = kstrdup(buf, GFP_KERNEL);
		if (!name)
			goto out;

		ontime_attr_init(cond->min_residency_attr, name, 0644,
				show_min_residency, store_min_residency);
		ontime_attrs[i++] = &cond->min_residency_attr.attr;

		step++;
		cond = cond->next;
	}

	ontime_group.attrs = ontime_attrs;

	ontime_kobj = kobject_create_and_add("ontime", parent);
	if (!ontime_kobj)
		goto out;

	if (sysfs_create_group(ontime_kobj, &ontime_group))
		goto out;

	return 0;

out:
	kfree(ontime_attrs);

	pr_err("ONTIME(%s): failed to create sysfs node\n", __func__);
	return -EINVAL;
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
	cond->min_residency = 8192;

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
	cond->min_residency = prop;
}

static int __init init_ontime(void)
{
	struct cpumask prev_cpus;
	struct ontime_cond *cond, *last;
	struct device_node *dn;
	int cpu, step = 0;

	dn = of_find_node_by_path("/cpus/ems");
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
