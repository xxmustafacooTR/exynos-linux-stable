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
#define GAMING_CONTROL_VERSION "0.6"

#define TASK_STARTED 1

enum game_opts {
	GAME_START,
	GAME_RUNNING,
	GAME_PAUSE
};

enum dvfs_id {
	DVFS_MIF = 0,
	DVFS_INT,
	DVFS_CPUCL0,
	DVFS_CPUCL1,
	DVFS_G3D,
	DVFS_INTCAM,
	DVFS_FSYS0,
	DVFS_CAM,
	DVFS_DISP_EVT1,
	DVFS_AUD,
	DVFS_IVA,
	DVFS_SCORE,
	DVFS_CP,
};

int gpex_clock_update_config_data_from_dt(void);
int gpex_clock_get_voltage(int clk);
int gpu_custom_min_clock(int gpu_min_clock);
int gpu_custom_max_clock(int gpu_max_clock);

#ifdef CONFIG_GAMING_CONTROL
extern bool gaming_mode;
extern void game_option(struct task_struct *tsk, enum game_opts opts);
extern bool battery_idle_gaming(void);
extern int thermal_bypass_gaming(void);
extern int fake_freq_gaming(int id);
extern unsigned long cal_dfs_check_gaming_mode(unsigned int id);
#else
static bool gaming_mode = 0;
static void game_option(struct task_struct *tsk, enum game_opts opts) {};
static bool battery_idle_gaming(void) {return false;};
static int thermal_bypass_gaming(void) {return 0;};
static int fake_freq_gaming(int id) {return 0;};
static unsigned long cal_dfs_check_gaming_mode(unsigned int id) {return 0;};
#endif /* CONFIG_GAMING_CONTROL */

#endif /* _GAMING_CONTROL_H_ */