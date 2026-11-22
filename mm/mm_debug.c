// SPDX-License-Identifier: GPL-2.0
/*
 * linux/mm/mm_debug.c
 *
 * Copyright (C) 2019 Samsung Electronics
 *
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <trace/systrace_mark.h>

/*
 * Written with __trace_puts() rather than trace_printk(): a trace_printk()
 * anywhere in the built-in kernel makes the tracer allocate its full ring
 * buffer at boot (1.4 MB per CPU) and print the "DEBUG kernel" banner.
 */
noinline void tracing_mark_write(int type, const char *str)
{
	char buf[SYSTRACE_MARK_BUF_SIZE + 16];
	int len;

	if (!tracing_is_on())
		return;

	switch (type) {
	case SYSTRACE_MARK_TYPE_BEGIN:
		len = scnprintf(buf, sizeof(buf), "B|%d|%s\n",
				current->tgid, str);
		break;
	case SYSTRACE_MARK_TYPE_END:
		len = scnprintf(buf, sizeof(buf), "E|%d|%s\n",
				current->tgid, str);
		break;
	default:
		return;
	}
	__trace_puts(_THIS_IP_, buf, len);
}
