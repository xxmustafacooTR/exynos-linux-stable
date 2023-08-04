/*
 * Simple kernel gaming control
 *
 * Copyright (C) 2019
 * Diep Quynh Nguyen <remilia.1505@gmail.com>
 * Mustafa Gökmen <mustafa.gokmen2004@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/binfmts.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pm_qos.h>
#include <linux/gaming_control.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-cpu_hotplug.h>
#include <dt-bindings/clock/exynos9810.h>
#include "../soc/samsung/cal-if/exynos9810/cmucal-vclk.h"
#include "../soc/samsung/cal-if/exynos9810/cmucal-node.h"

#define DEFAULT_CPU_LIMIT 1794000
#define TEMP_EMULATION 20000

extern unsigned long arg_cpu_max_c1;
extern unsigned long arg_cpu_min_c1;
extern unsigned long arg_cpu_max_c2;
extern unsigned long arg_cpu_min_c2;
extern unsigned long arg_gpu_min;
extern unsigned long arg_gpu_max;
extern int exynos_cpufreq_update_volt_table(void);
extern void exynos_cpufreq_set_gaming_mode(void);

/* PM QoS implementation */
static struct pm_qos_request gaming_control_min_int_qos;
static struct pm_qos_request gaming_control_min_mif_qos;
static struct pm_qos_request gaming_control_min_big_qos;
static struct pm_qos_request gaming_control_max_big_qos;
static struct pm_qos_request gaming_control_min_little_qos;
static struct pm_qos_request gaming_control_max_little_qos;
static unsigned int min_int_freq = 534000;
static unsigned int min_mif_freq = 1794000;
static unsigned int min_little_freq = 1456000;
static unsigned int max_little_freq = 2002000;
static unsigned int min_big_freq = 1469000;
static unsigned int max_big_freq = 2886000;
static unsigned int min_gpu_freq = 598000;
static unsigned int max_gpu_freq = 598000;
static unsigned int custom_little_freq, custom_little_voltage, custom_big_freq, custom_big_voltage, custom_gpu_freq, custom_gpu_voltage = 0;
static unsigned int back_little_freq, back_little_voltage, back_big_freq, back_big_voltage, back_gpu_freq, back_gpu_voltage = 0;
static int nr_running_games = 0;
static bool always_on = 0;
static bool battery_idle = 0;
static int thermal_bypass = 0;
static bool gaming_mode_initialized = 0;

bool gaming_mode;

char games_list[GAME_LIST_LENGTH] = {0};
pid_t games_pid[NUM_SUPPORTED_RUNNING_GAMES] = {
	[0 ... (NUM_SUPPORTED_RUNNING_GAMES - 1)] = -1
};

static inline void set_gaming_mode(bool mode, bool force)
{
	unsigned int little_max, little_min, big_max, big_min, gpu_max, gpu_min;

	if (always_on)
		mode = 1;

	if (mode == gaming_mode && !force)
		return;
	else
		gaming_mode = mode;

	if (!mode)
		gaming_mode_initialized = 0;

	little_max = max_little_freq;
	little_min = min_little_freq;
	big_max = max_big_freq;
	big_min = min_big_freq;
	gpu_max = max_gpu_freq;
	gpu_min = min_gpu_freq;

	if (custom_little_freq) {
		if (custom_little_freq <= arg_cpu_min_c1)
			little_max = little_min = arg_cpu_min_c1;
		else if (custom_little_freq >= arg_cpu_max_c1)
			little_max = little_min = arg_cpu_max_c1;
	}

	if (!back_little_freq)
			back_little_freq = little_max;

	if (custom_little_voltage && mode && !back_little_voltage) {
		back_little_voltage = fvmap_read(DVFS_CPUCL0, READ_VOLT, back_little_freq);
		fvmap_patch(DVFS_CPUCL0, back_little_freq, custom_little_voltage);
	} else if (!mode && back_little_voltage) {
		fvmap_patch(DVFS_CPUCL0, back_little_freq, back_little_voltage);
		back_little_freq = back_little_voltage = 0;
	}

	if (custom_big_freq) {
		if (custom_big_freq <= arg_cpu_min_c2) {
			big_max = big_min = arg_cpu_min_c2;
		} else if (custom_big_freq >= arg_cpu_max_c2) {
			if (mode || DEFAULT_CPU_LIMIT > arg_cpu_max_c2)
				big_max = big_min = arg_cpu_max_c2;
			else
				big_max = big_min = DEFAULT_CPU_LIMIT;
		}
	}

	if (!back_big_freq)
			back_big_freq = big_max;

	if (custom_big_voltage && mode && !back_big_voltage) {
		back_big_voltage = fvmap_read(DVFS_CPUCL1, READ_VOLT, back_big_freq);
		fvmap_patch(DVFS_CPUCL1, back_big_freq, custom_big_voltage);
	} else if (!mode && back_big_voltage) {
		fvmap_patch(DVFS_CPUCL1, back_big_freq, back_big_voltage);
		back_big_freq = back_big_voltage = 0;
	}

	if (custom_gpu_freq) {
		if (custom_gpu_freq <= arg_gpu_min)
			gpu_max = gpu_min = arg_gpu_min;
		else if (custom_gpu_freq >= arg_gpu_max)
			gpu_max = gpu_min = arg_gpu_max;
	}

	if (!back_gpu_freq)
			back_gpu_freq = gpu_max;

	if (custom_gpu_voltage && mode && !back_gpu_voltage) {
		back_gpu_voltage = gpex_clock_get_voltage(back_gpu_freq);
		fvmap_patch(DVFS_G3D, back_gpu_freq, custom_gpu_voltage);
	} else if (!mode && back_gpu_voltage) {
		fvmap_patch(DVFS_G3D, back_gpu_freq, back_gpu_voltage);
		back_gpu_freq = back_gpu_voltage = 0;
	}

	gpex_clock_update_config_data_from_dt();
	exynos_cpufreq_update_volt_table();
	exynos_cpufreq_set_gaming_mode();

	pm_qos_update_request(&gaming_control_min_int_qos, mode && min_int_freq ? min_int_freq : PM_QOS_DEVICE_THROUGHPUT_DEFAULT_VALUE);
	pm_qos_update_request(&gaming_control_min_mif_qos, mode && min_mif_freq ? min_mif_freq : PM_QOS_BUS_THROUGHPUT_DEFAULT_VALUE);
	pm_qos_update_request(&gaming_control_min_little_qos, mode && little_min ? little_min : PM_QOS_CLUSTER0_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_update_request(&gaming_control_max_little_qos, mode && little_max ? little_max : PM_QOS_CLUSTER0_FREQ_MAX_DEFAULT_VALUE);
	pm_qos_update_request(&gaming_control_min_big_qos, mode && big_min ? big_min : PM_QOS_CLUSTER1_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_update_request(&gaming_control_max_big_qos, mode && big_max ? big_max : PM_QOS_CLUSTER1_FREQ_MAX_DEFAULT_VALUE);

	gpu_custom_max_clock(mode ? gpu_max : 0);
	gpu_custom_min_clock(mode ? gpu_min : 0);

	if (mode)
		gaming_mode_initialized = 1;

	if (custom_little_freq) {
		__cal_dfs_set_rate(ACPM_DVFS_CPUCL0, mode ? custom_little_freq : little_max);
		__cal_dfs_set_rate(VCLK_CLUSTER0, (mode ? custom_little_freq : little_max) * 1000);
		__cal_dfs_set_rate(PLL_CPUCL0, (mode ? custom_little_freq : little_max) * 1000);
	}

	if (custom_big_freq) {
		__cal_dfs_set_rate(ACPM_DVFS_CPUCL1, mode ? custom_big_freq : big_max);
		__cal_dfs_set_rate(VCLK_CLUSTER1, (mode ? custom_big_freq : big_max) * 1000);
		__cal_dfs_set_rate(PLL_CPUCL1, (mode ? custom_big_freq : big_max) * 1000);
	}

	if (custom_gpu_freq) {
		__cal_dfs_set_rate(ACPM_DVFS_G3D, mode ? custom_gpu_freq : gpu_max);
		__cal_dfs_set_rate(VCLK_GPU, (mode ? custom_gpu_freq : gpu_max) * 1000);
		__cal_dfs_set_rate(PLL_G3D, (mode ? custom_gpu_freq : gpu_max) * 1000);
	}
}

unsigned long cal_dfs_check_gaming_mode(unsigned int id) {
	unsigned long ret = 0;

	if (!gaming_mode_initialized)
		return ret;

	switch (id) {
	/* LITTLE */
	case ACPM_DVFS_CPUCL0:
		ret = custom_little_freq;
		break;
	/* BIG */
	case ACPM_DVFS_CPUCL1:
		ret = custom_big_freq;
		break;
	/* GPU */
	case ACPM_DVFS_G3D:
		ret = custom_gpu_freq;
		break;
	default:
		break;
	}

	return ret;
}

int fake_freq_gaming(int id) {
	int ret = 0;

	if (!gaming_mode)
		return ret;

	switch (id) {
	/* LITTLE */
	case ACPM_DVFS_CPUCL0:
	case DVFS_CPUCL0:
		ret = back_little_freq;
		break;
	/* BIG */
	case ACPM_DVFS_CPUCL1:
	case DVFS_CPUCL1:
		ret = back_big_freq;
		break;
	/* GPU */
	case ACPM_DVFS_G3D:
	case DVFS_G3D:
		ret = back_gpu_freq;
		break;
	default:
		break;
	}

	return ret;
}

bool battery_idle_gaming(void) {
	if (gaming_mode && battery_idle)
		return 1;

	return 0;
}

int thermal_bypass_gaming(void) {
	if (gaming_mode && thermal_bypass) {
		if (thermal_bypass == 1)
			return TEMP_EMULATION;
		else
			return thermal_bypass;
	}

	return 0;
}

static inline void store_game_pid(pid_t pid)
{
	int i;

	for (i = 0; i < NUM_SUPPORTED_RUNNING_GAMES; i++) {
		if (games_pid[i] == -1) {
			games_pid[i] = pid;
			nr_running_games++;
			break;
		}
	}
}

static inline int check_game_pid(pid_t pid)
{
	int i;

	for (i = 0; i < NUM_SUPPORTED_RUNNING_GAMES; i++) {
		if (games_pid[i] != -1) {
			if (games_pid[i] == pid)
				return 1;
		}
	}
	return 0;
}

static inline void clear_dead_pids(void)
{
	int i;

	for (i = 0; i < NUM_SUPPORTED_RUNNING_GAMES; i++) {
		if (games_pid[i] != -1) {
			rcu_read_lock();
			if (!find_task_by_vpid(games_pid[i])) {
				games_pid[i] = -1;
				nr_running_games--;
			}
			rcu_read_unlock();
		}
	}

	/* If there's no running games, turn off game mode */
	if (nr_running_games == 0)
		set_gaming_mode(false, false);
}

static inline void removeSubstringAfterChar(char *str, char ch) {
    char *ptr = str;
    while (*ptr != '\0') {
        if (*ptr == ch) {
            *(ptr) = '\0';
            return;
        }
        ptr++;
    }
}

static int check_for_games(struct task_struct *tsk)
{
	char *cmdline;
	int ret;

	cmdline = kmalloc(GAME_LIST_LENGTH, GFP_KERNEL);
	if (!cmdline)
		return 0;

	ret = get_cmdline(tsk, cmdline, GAME_LIST_LENGTH);
	if (ret == 0) {
		kfree(cmdline);
		return 0;
	}

	/* clean extra process identifier */
	removeSubstringAfterChar(cmdline, ':');

	/* Invalid Android process name. Bail out */
	if (!strcmp(cmdline, "android") || strlen(cmdline) < 7) {
		kfree(cmdline);
		return 0;
	}

	/* tsk isn't a game. Bail out */
	if (strstr(games_list, cmdline) == NULL) {
		kfree(cmdline);
		return 0;
	}

	kfree(cmdline);

	return 1;
}

void game_option(struct task_struct *tsk, enum game_opts opts)
{
	pid_t pid;
	int ret;

	/* Remove all zombie tasks PIDs */
	clear_dead_pids();
	
	if(always_on) {
		set_gaming_mode(true, false);
		return;
	}

	ret = check_for_games(tsk);
	if (!ret)
		return;

	switch (opts) {
	case GAME_START:
	case GAME_RUNNING:
		set_gaming_mode(true, false);
		
		pid = task_pid_vnr(tsk);

		if (tsk->app_state == TASK_STARTED || check_game_pid(pid))
			break;

		store_game_pid(pid);
		tsk->app_state = TASK_STARTED;
		break;
	case GAME_PAUSE:
		set_gaming_mode(false, false);
		break;
	default:
		break;
	}
}

/* Show added games list */
static ssize_t game_packages_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", games_list);
}

/* Store added games list */
static ssize_t game_packages_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	char games[GAME_LIST_LENGTH];

	sscanf(buf, "%s", games);
	sprintf(games_list, "%s", buf);

	return count;
}

/* Show/Store value */
#define attr_value(type)						\
static ssize_t type##_store(struct kobject *kobj,				\
		struct kobj_attribute *attr, const char *buf, size_t count)	\
{										\
	unsigned int value;							\
										\
	sscanf(buf, "%u\n", &value);						\
	type = value;								\
										\
	set_gaming_mode(gaming_mode, gaming_mode);						\
										\
	return count;								\
}		\
	\
static ssize_t type##_show(struct kobject *kobj,		\
		struct kobj_attribute *attr, char *buf)		\
{								\
	return sprintf(buf, "%u\n", type);			\
}								\
	\
static struct kobj_attribute type##_attribute =				\
	__ATTR(type, 0644, type##_show, type##_store);

attr_value(always_on);
attr_value(battery_idle);
attr_value(thermal_bypass);
attr_value(min_int_freq);
attr_value(min_mif_freq);
attr_value(min_little_freq);
attr_value(max_little_freq);
attr_value(min_big_freq);
attr_value(max_big_freq);
attr_value(min_gpu_freq);
attr_value(max_gpu_freq);
attr_value(custom_little_freq);
attr_value(custom_little_voltage);
attr_value(custom_big_freq);
attr_value(custom_big_voltage);
attr_value(custom_gpu_freq);
attr_value(custom_gpu_voltage);

static ssize_t status_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
    ssize_t len = 0;
	int i;

	len += sprintf(buf + len, "%d\n\n", nr_running_games);

	for (i = 0; i < NUM_SUPPORTED_RUNNING_GAMES; i++) {
		if (games_pid[i] != -1)
			len += sprintf(buf + len, "%d\n", games_pid[i]);
	}

    buf[len] = '\0';

    return len;
}

static ssize_t version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", GAMING_CONTROL_VERSION);
}

static struct kobj_attribute status_attribute =
	__ATTR(status, 0444, status_show, NULL);

static struct kobj_attribute version_attribute =
	__ATTR(version, 0444, version_show, NULL);

static struct kobj_attribute game_packages_attribute =
	__ATTR(game_packages, 0644, game_packages_show, game_packages_store);

static struct attribute *gaming_control_attributes[] = {
	&game_packages_attribute.attr,
	&status_attribute.attr,
	&version_attribute.attr,
	&always_on_attribute.attr,
	&battery_idle_attribute.attr,
	&thermal_bypass_attribute.attr,
	&min_int_freq_attribute.attr,
	&min_mif_freq_attribute.attr,
	&min_little_freq_attribute.attr,
	&max_little_freq_attribute.attr,
	&min_big_freq_attribute.attr,
	&max_big_freq_attribute.attr,
	&min_gpu_freq_attribute.attr,
	&max_gpu_freq_attribute.attr,
	&custom_little_freq_attribute.attr,
	&custom_little_voltage_attribute.attr,
	&custom_big_freq_attribute.attr,
	&custom_big_voltage_attribute.attr,
	&custom_gpu_freq_attribute.attr,
	&custom_gpu_voltage_attribute.attr,
	NULL
};

static struct attribute_group gaming_control_control_group = {
	.attrs = gaming_control_attributes,
};

static struct kobject *gaming_control_kobj;

static int gaming_control_init(void)
{
	int sysfs_result;

	pm_qos_add_request(&gaming_control_min_int_qos, PM_QOS_DEVICE_THROUGHPUT, PM_QOS_DEVICE_THROUGHPUT_DEFAULT_VALUE);
	pm_qos_add_request(&gaming_control_min_mif_qos, PM_QOS_BUS_THROUGHPUT, PM_QOS_BUS_THROUGHPUT_DEFAULT_VALUE);
	pm_qos_add_request(&gaming_control_min_little_qos, PM_QOS_CLUSTER0_FREQ_MIN, PM_QOS_CLUSTER0_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_add_request(&gaming_control_max_little_qos, PM_QOS_CLUSTER0_FREQ_MAX, PM_QOS_CLUSTER0_FREQ_MAX_DEFAULT_VALUE);
	pm_qos_add_request(&gaming_control_min_big_qos, PM_QOS_CLUSTER1_FREQ_MIN, PM_QOS_CLUSTER1_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_add_request(&gaming_control_max_big_qos, PM_QOS_CLUSTER1_FREQ_MAX, PM_QOS_CLUSTER1_FREQ_MAX_DEFAULT_VALUE);
	
	gaming_control_kobj = kobject_create_and_add("gaming_control", kernel_kobj);
	if (!gaming_control_kobj) {
		pr_err("%s gaming_control kobject create failed!\n", __FUNCTION__);
		return -ENOMEM;
	}

	sysfs_result = sysfs_create_group(gaming_control_kobj,
			&gaming_control_control_group);

	if (sysfs_result) {
		pr_info("%s gaming_control sysfs create failed!\n", __FUNCTION__);
		kobject_put(gaming_control_kobj);
	}

	return sysfs_result;
}


static void gaming_control_exit(void)
{
	pm_qos_remove_request(&gaming_control_min_int_qos);
	pm_qos_remove_request(&gaming_control_min_mif_qos);
	pm_qos_remove_request(&gaming_control_min_little_qos);
	pm_qos_remove_request(&gaming_control_max_little_qos);
	pm_qos_remove_request(&gaming_control_min_big_qos);
	pm_qos_remove_request(&gaming_control_max_big_qos);

	if (gaming_control_kobj != NULL)
		kobject_put(gaming_control_kobj);
}

module_init(gaming_control_init);
module_exit(gaming_control_exit);