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

#ifndef _GAMING_CONTROL_H_
#define _GAMING_CONTROL_H_

#define GAME_LIST_LENGTH 1024
#define NUM_SUPPORTED_RUNNING_GAMES 20
#define GAMING_CONTROL_VERSION "0.4"

#define TASK_STARTED 1

enum game_opts {
	GAME_START,
	GAME_RUNNING,
	GAME_PAUSE
};

extern unsigned int min_int_freq;
extern unsigned int min_mif_freq;
extern unsigned int min_little_freq;
extern unsigned int max_little_freq;
extern unsigned int min_big_freq;
extern unsigned int max_big_freq;
extern unsigned int min_gpu_freq;
extern unsigned int max_gpu_freq;

int gpu_custom_min_clock(int gpu_min_clock);
int gpu_custom_max_clock(int gpu_max_clock);

#ifdef CONFIG_GAMING_CONTROL
extern void game_option(struct task_struct *tsk, enum game_opts opts);
extern bool gaming_mode;
extern bool battery_idle_gaming(void);
extern void exynos_cpufreq_set_gaming_mode(void);
#else
static void game_option(struct task_struct *tsk, enum game_opts opts) {}
static bool gaming_mode = 0;
extern bool battery_idle_gaming(void) {return false;};
extern void exynos_cpufreq_set_gaming_mode(void) {return false;};
#endif /* CONFIG_GAMING_CONTROL */

#endif /* _GAMING_CONTROL_H_ */