/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

enum stune_group {
	STUNE_ROOT,
	STUNE_FOREGROUND,
	STUNE_BACKGROUND,
	STUNE_TOPAPP,
	STUNE_RT,
	STUNE_GROUP_COUNT,
};

#ifdef CONFIG_SCHED_EMS
/* prefer perf */
extern int kernel_prefer_perf(int grp_idx);
extern void request_kernel_prefer_perf(int grp_idx, int enable);
#else
static inline int kernel_prefer_perf(int grp_idx) { }
static inline void request_kernel_prefer_perf(int grp_idx, int enable) { }
#endif
