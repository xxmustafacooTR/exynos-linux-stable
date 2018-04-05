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

#define cpu_selected(cpu)	(cpu >= 0)

static int task_util(struct task_struct *p)
{
	return p->se.avg.util_avg;
}

static int cpu_util_wake(int cpu, struct task_struct *p)
{
	unsigned long util, capacity;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !p->se.avg.last_update_time)
		return cpu_util(cpu);

	capacity = capacity_orig_of(cpu);
	util = max_t(long, cpu_rq(cpu)->cfs.avg.util_avg - task_util(p), 0);

	return (util >= capacity) ? capacity : util;
}

struct energy_table {
	struct capacity_state *states;
	unsigned int nr_states;
};

DEFINE_PER_CPU(struct energy_table, energy_table);

struct eco_env {
	struct task_struct *p;

	int prev_cpu;
	int best_cpu;
	int backup_cpu;
};

static void find_eco_target(struct eco_env *eenv)
{
	struct task_struct *p = eenv->p;
	unsigned long best_min_cap_orig = ULONG_MAX;
	unsigned long backup_min_cap_orig = ULONG_MAX;
	unsigned long best_spare_cap = 0;
	int backup_idle_cstate = INT_MAX;
	int best_cpu = -1;
	int backup_cpu = -1;
	int cpu;

	/*
	 * It is meaningless to find an energy cpu when the energy table is
	 * not created or has not been created yet.
	 */
	if (!per_cpu(energy_table, eenv->prev_cpu).nr_states)
		return;

	rcu_read_lock();

	for_each_cpu_and(cpu, &p->cpus_allowed, cpu_active_mask) {
		unsigned long capacity_orig = capacity_orig_of(cpu);
		unsigned long wake_util, new_util;

		wake_util = cpu_util_wake(cpu, p);
		new_util = wake_util + task_util(p);

		/* checking prev cpu is meaningless */
		if (eenv->prev_cpu == cpu)
			continue;

		/* skip over-capacity cpu */
		if (new_util > capacity_orig)
			continue;

		/*
		 * According to the criteria determined by the LBT(Load
		 * Balance trigger), the cpu that becomes overutilized when
		 * the task is assigned is skipped.
		 */
		if (lbt_bring_overutilize(cpu, p))
			continue;

		/*
		 * Backup target) shallowest idle cpu among min-cap cpu
		 *
		 * In general, assigning a task to an idle cpu is
		 * disadvantagerous in energy. To minimize the energy increase
		 * associated with selecting idle cpu, choose a cpu that is
		 * in the lowest performance and shallowest idle state.
		 */
		if (idle_cpu(cpu)) {
			int idle_idx;

			if (backup_min_cap_orig < capacity_orig)
				continue;

			idle_idx = idle_get_state_idx(cpu_rq(cpu));
			if (backup_idle_cstate <= idle_idx)
				continue;

			backup_min_cap_orig = capacity_orig;
			backup_idle_cstate = idle_idx;
			backup_cpu = cpu;
			continue;
		}

		/*
		 * Best target) biggest spare cpu among min-cap cpu
		 *
		 * Select the cpu with the biggest spare capacity to maintain
		 * frequency as possible without waking up idle cpu. Also, to
		 * maximize the use of energy-efficient cpu, we choose the
		 * lowest performance cpu.
		 */
		if (best_min_cap_orig < capacity_orig)
			continue;

		if (best_spare_cap > (capacity_orig - new_util))
			continue;

		best_spare_cap = capacity_orig - new_util;
		best_min_cap_orig = capacity_orig;
		best_cpu = cpu;
	}

	rcu_read_unlock();

	eenv->best_cpu = best_cpu;
	eenv->backup_cpu = backup_cpu;
}

static unsigned int calculate_energy(struct task_struct *p, int target_cpu)
{
	unsigned long util[NR_CPUS] = {0, };
	unsigned int total_energy = 0;
	int cpu;

	/*
	 * 0. Calculate utilization of the entire active cpu when task
	 *    is assigned to target cpu.
	 */
	for_each_cpu(cpu, cpu_active_mask) {
		util[cpu] = cpu_util_wake(cpu, p);

		if (unlikely(cpu == target_cpu))
			util[cpu] += task_util(p);
	}

	for_each_possible_cpu(cpu) {
		struct energy_table *table;
		unsigned long max_util = 0, util_sum = 0;
		unsigned long capacity;
		int i, cap_idx;

		/* Compute coregroup energy with only one cpu per coregroup */
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		/*
		 * 1. The cpu in the coregroup has same capacity and the
		 *    capacity depends on the cpu that has the biggest
		 *    utilization. Find biggest utilization in the coregroup
		 *    to know what capacity the cpu will have.
		 */
		for_each_cpu(i, cpu_coregroup_mask(cpu))
			if (util[i] > max_util)
				max_util = util[i];

		/*
		 * 2. Find the capacity according to biggest utilization in
		 *    coregroup.
		 */
		table = &per_cpu(energy_table, cpu);
		cap_idx = table->nr_states - 1;
		for (i = 0; i < table->nr_states; i++) {
			if (table->states[i].cap >= max_util) {
				capacity = table->states[i].cap;
				cap_idx = i;
				break;
			}
		}

		/*
		 * 3. Get the utilization sum of coregroup. Since cpu
		 *    utilization of CFS reflects the performance of cpu,
		 *    normalize the utilization to calculate the amount of
		 *    cpu usuage that excludes cpu performance.
		 */
		for_each_cpu(i, cpu_coregroup_mask(cpu)) {
			/* utilization with task exceeds max capacity of cpu */
			if (util[i] >= capacity) {
				util_sum += SCHED_CAPACITY_SCALE;
				continue;
			}

			/* normalize cpu utilization */
			util_sum += (util[i] << SCHED_CAPACITY_SHIFT) / capacity;
		}

		/*
		 * 4. compute active energy
		 */
		total_energy += util_sum * table->states[cap_idx].power;
	}

	return total_energy;
}

static int select_eco_cpu(struct eco_env *eenv)
{
	unsigned int prev_energy, best_energy, backup_energy;
	unsigned int temp_energy;
	int temp_cpu;
	int eco_cpu = eenv->prev_cpu;
	int margin;

	prev_energy = calculate_energy(eenv->p, eenv->prev_cpu);

	/*
	 * find_eco_target() may not find best or backup cup. Ignore unfound
	 * cpu, and if both are found, select a cpu that consumes less energy
	 * when assigning task.
	 */
	best_energy = backup_energy = UINT_MAX;

	if (cpu_selected(eenv->best_cpu))
		best_energy = calculate_energy(eenv->p, eenv->best_cpu);

	if (cpu_selected(eenv->backup_cpu))
		backup_energy = calculate_energy(eenv->p, eenv->backup_cpu);

	if (best_energy < backup_energy) {
		temp_energy = best_energy;
		temp_cpu = eenv->best_cpu;
	} else {
		temp_energy = backup_energy;
		temp_cpu = eenv->backup_cpu;
	}

	/*
	 * Compare prev cpu to target cpu among best and backup cpu to determine
	 * whether keeping the task on PREV CPU and sending the task to TARGET
	 * CPU is beneficial for energy.
	 */
	if (temp_energy < prev_energy) {
		/*
		 * Compute the dead-zone margin used to prevent too many task
		 * migrations with negligible energy savings.
		 * An energy saving is considered meaningful if it reduces the
		 * energy consumption of PREV CPU candidate by at least ~1.56%.
		 */
		margin = prev_energy >> 6;
		if ((prev_energy - temp_energy) < margin)
			goto out;

		eco_cpu = temp_cpu;
	}

out:
	trace_ems_select_eco_cpu(eenv->p, eco_cpu,
			eenv->prev_cpu, eenv->best_cpu, eenv->backup_cpu,
			prev_energy, best_energy, backup_energy);
	return eco_cpu;
}

static int
select_energy_cpu(struct task_struct *p, int prev_cpu, int sd_flag, int sync)
{
	struct sched_domain *sd = NULL;
	int cpu = smp_processor_id();
	struct eco_env eenv = {
		.p = p,
		.prev_cpu = prev_cpu,
		.best_cpu = -1,
		.backup_cpu = -1,
	};

	if (!sched_feat(ENERGY_AWARE))
		return -1;

	/*
	 * Energy-aware wakeup placement on overutilized cpu is hard to get
	 * energy gain.
	 */
	rcu_read_lock();
	sd = rcu_dereference_sched(cpu_rq(prev_cpu)->sd);
	if (!sd || sd->shared->overutilized) {
		rcu_read_unlock();
		return -1;
	}
	rcu_read_unlock();

	/*
	 * We cannot do energy-aware wakeup placement sensibly for tasks
	 * with 0 utilization, so let them be placed according to the normal
	 * strategy.
	 */
	if (!task_util(p))
		return -1;

	if (sysctl_sched_sync_hint_enable && sync)
		if (cpumask_test_cpu(cpu, &p->cpus_allowed))
			return cpu;

	/*
	 * Find eco-friendly target.
	 * After selecting the best and backup cpu according to strategy, we
	 * choose a cpu that is energy efficient compared to prev cpu.
	 */
	find_eco_target(&eenv);
	if (eenv.best_cpu < 0 && eenv.backup_cpu < 0)
		return prev_cpu;

	return select_eco_cpu(&eenv);
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
