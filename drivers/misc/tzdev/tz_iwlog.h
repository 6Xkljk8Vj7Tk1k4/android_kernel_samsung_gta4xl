/*
 * Copyright (C) 2013-2016 Samsung Electronics, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __TZ_IWLOG_H__
#define __TZ_IWLOG_H__

#include <linux/types.h>

#if defined(CONFIG_TZLOG)
int tz_iwlog_initialize(void);
void tz_iwlog_read_buffers(void);
void tz_iwlog_get_counts(unsigned int cpu, uint32_t *write_count, uint32_t *read_count);
#else
static inline int tz_iwlog_initialize(void)
{
	return 0;
}

static inline void tz_iwlog_read_buffers(void)
{
}

static inline void tz_iwlog_get_counts(unsigned int cpu, uint32_t *write_count,
		uint32_t *read_count)
{
	*write_count = 0;
	*read_count = 0;
}
#endif

#endif /* __TZ_IWLOG_H__ */
