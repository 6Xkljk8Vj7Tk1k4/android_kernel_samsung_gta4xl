/*
 * Copyright (C) 2012-2018, Samsung Electronics Co., Ltd.
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

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/smpboot.h>
#include <linux/string.h>
#include <linux/version.h>

#include "sysdep.h"
#include "tzdev.h"
#include "tzlog.h"
#include "tz_iwlog.h"
#include "tz_iwservice.h"
#include "tz_iwsock.h"
#include "tz_kthread_pool.h"
#include "tz_mem.h"

enum {
	KTHREAD_SHOULD_SLEEP,
	KTHREAD_SHOULD_RUN,
	KTHREAD_SHOULD_STOP,
	KTHREAD_SHOULD_PARK,
};

static atomic_t tz_kthread_pool_fini_done = ATOMIC_INIT(0);

static DEFINE_PER_CPU(struct task_struct *, worker);
static DECLARE_WAIT_QUEUE_HEAD(tz_cmd_waitqueue);
static atomic_t tz_nr_cmds = ATOMIC_INIT(0);

/*
 * Diagnostics for a secure world that keeps asking for a CPU (2026-11-21:
 * tz_worker_thread/4 entered it 5600 times a second for 38 minutes, the request
 * mask staying 0x10). A streak is a run of entries by one worker without a sleep
 * in between, each ending with a non-empty request mask. A streak longer than
 * /sys/module/tzdev/parameters/worker_report_ms (0: never) is reported in the
 * kernel log once, with the state below, and its end is reported too. The state
 * can be read at any time from /sys/module/tzdev/parameters/worker_diag; it
 * keeps the longest streak since boot, for when the log has wrapped. Real work
 * in the secure world makes long streaks too, of entries that take milliseconds
 * each; the entries of 2026-11-21 took 160 us.
 */
#define TZ_WORKER_GAP_NS	(100ULL * NSEC_PER_MSEC)

struct tz_worker_stat {
	u64 entries;		/* entries into the secure world since boot */
	u64 swd_ns;		/* time spent in them */
	u64 last_end;		/* ktime_get_ns() after the last entry */
	u64 streak;		/* entries in the current streak */
	u64 streak_start;	/* ktime_get_ns() before its first entry */
	u64 streak_swd_ns;	/* time spent in the secure world during it */
	u64 streak_nonzero;	/* entries in it that returned x0 != 0 */
	u64 longest_ns;		/* longest streak since boot */
	u64 longest_entries;	/* entries in it */
	uint32_t ret[4];	/* x0-x3 returned by the last entry */
	bool reported;
};

static DEFINE_PER_CPU(struct tz_worker_stat, tz_worker_stat);

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "tzdev."

static unsigned int tz_worker_report_ms = 10000;
module_param_named(worker_report_ms, tz_worker_report_ms, uint, 0644);

static int tz_kthread_pool_cmd_get(void)
{
	return atomic_dec_if_positive(&tz_nr_cmds) >= 0;
}

static int tz_kthread_pool_should_wake(unsigned long cpu)
{
	unsigned long sk_cpu_mask;
	cpumask_t cpu_mask;
	cpumask_t outstanding_cpu_mask;
	cpumask_t requested_cpu_mask;

	if (kthread_should_stop()) {
		tzdev_kthread_info("Requested kthread stop on cpu = %lx\n", cpu);
		return KTHREAD_SHOULD_STOP;
	}

	sk_cpu_mask = tz_iwservice_get_cpu_mask();

	cpumask_copy(&requested_cpu_mask, to_cpumask(&sk_cpu_mask));
	cpumask_and(&cpu_mask, &requested_cpu_mask, cpu_online_mask);
	cpumask_andnot(&outstanding_cpu_mask, &requested_cpu_mask, &cpu_mask);

	tzdev_kthread_info("cpu mask iwservice = %lx\n",
			sk_cpu_mask);
	tzdev_kthread_info("cpu mask requested = %*pb\n",
			cpumask_pr_args(&requested_cpu_mask));
	tzdev_kthread_info("cpu mask effective = %*pb\n",
			cpumask_pr_args(&cpu_mask));
	tzdev_kthread_info("cpu mask outstanding = %*pb\n",
			cpumask_pr_args(&outstanding_cpu_mask));

	if (kthread_should_park()) {
		if (cpu_isset(cpu, cpu_mask))
			tz_kthread_pool_cmd_send();

		tzdev_kthread_info("Requested kthread park on cpu = %lx\n", cpu);
		return KTHREAD_SHOULD_PARK;
	}

	if (cpu_isset(cpu, cpu_mask)) {
		tzdev_kthread_debug("Direct cpu hit = %lu\n", cpu);
		tz_kthread_pool_cmd_get();
		return KTHREAD_SHOULD_RUN;
	} else if (!cpumask_empty(&outstanding_cpu_mask)) {
		tzdev_kthread_debug("No proper cpus to satisfy requested affinity\n");
		tz_kthread_pool_cmd_get();
		return KTHREAD_SHOULD_RUN;
	}

	if (tz_kthread_pool_cmd_get()) {
		tzdev_kthread_debug("Handle initial cmd on cpu = %lu\n", cpu);
		return KTHREAD_SHOULD_RUN;
	}

	return KTHREAD_SHOULD_SLEEP;
}

static int tz_kthread_pool_wake_function(wait_queue_t *wait, unsigned int mode, int sync,
				void *key)
{
	unsigned long cpu = (unsigned long)key;

	/*
	 * Avoid a wakeup if enter to SWd should be done from
	 * user thread context
	 */
	if (cpu == raw_smp_processor_id())
		return 0;
	return autoremove_wake_function(wait, mode, sync, key);
}

static int tz_kthread_pool_wait_for_event(unsigned long cpu)
{
	int ret;
	DEFINE_WAIT_FUNC(wait, tz_kthread_pool_wake_function);

	for (;;) {
		prepare_to_wait(&tz_cmd_waitqueue, &wait, TASK_UNINTERRUPTIBLE);
		ret = tz_kthread_pool_should_wake(cpu);
		if (ret != KTHREAD_SHOULD_SLEEP)
			break;
		schedule();
	}
	finish_wait(&tz_cmd_waitqueue, &wait);

	return ret;
}

static void tz_kthread_pool_wake_up_all_but(unsigned long cpu)
{
	__wake_up(&tz_cmd_waitqueue, TASK_NORMAL, 0, (void *)cpu);
}

void tz_kthread_pool_wake_up_all(void)
{
	tz_kthread_pool_wake_up_all_but(NR_CPUS);
}

static int tz_worker_diag_print(char *buf, size_t size)
{
	const struct tz_worker_stat *st;
	uint32_t log_wr, log_rd;
	unsigned int cpu;
	int len;

	len = scnprintf(buf, size, "cpu_mask %#lx user_cpu_mask %#lx cmds %d online %*pbl\n",
			tz_iwservice_get_cpu_mask(), tz_iwservice_get_user_cpu_mask(),
			atomic_read(&tz_nr_cmds), cpumask_pr_args(cpu_online_mask));
	len += scnprintf(buf + len, size - len,
			"cpu entries swd_ms streak streak_ms longest_ms longest_entries x0 x1 x2 x3 log_wr log_rd\n");

	for_each_possible_cpu(cpu) {
		st = per_cpu_ptr(&tz_worker_stat, cpu);
		tz_iwlog_get_counts(cpu, &log_wr, &log_rd);
		len += scnprintf(buf + len, size - len,
				"%u %llu %llu %llu %llu %llu %llu %#x %#x %#x %#x %u %u\n",
				cpu, st->entries, div_u64(st->swd_ns, NSEC_PER_MSEC),
				st->streak,
				st->streak ? div_u64(st->last_end - st->streak_start, NSEC_PER_MSEC) : 0,
				div_u64(st->longest_ns, NSEC_PER_MSEC), st->longest_entries,
				st->ret[0], st->ret[1], st->ret[2], st->ret[3], log_wr, log_rd);
	}

	len += tz_iwsock_snprint_state(buf + len, size - len);

	return len;
}

static void tz_worker_report(unsigned int cpu, struct tz_worker_stat *st)
{
	char *buf, *p, *line;

	tzdev_print(0, "tz_worker_thread/%u: the secure world asked for CPUs %#lx after each of %llu entries in %llu ms (%llu us per entry inside, x0 != 0 in %llu)\n",
			cpu, tz_iwservice_get_cpu_mask(), st->streak,
			div_u64(st->last_end - st->streak_start, NSEC_PER_MSEC),
			div_u64(div64_u64(st->streak_swd_ns, st->streak), NSEC_PER_USEC),
			st->streak_nonzero);

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	tz_worker_diag_print(buf, PAGE_SIZE);

	p = buf;
	while ((line = strsep(&p, "\n")) && *line)
		tzdev_print(0, "tz_worker_thread/%u: %s\n", cpu, line);

	kfree(buf);
}

/* Enter the secure world from the worker of @cpu and account for the entry. */
static void tz_worker_enter_swd(unsigned int cpu)
{
	struct tz_worker_stat *st = per_cpu_ptr(&tz_worker_stat, cpu);
	struct tzdev_smc_data res;
	unsigned long sk_cpu_mask;
	unsigned int report_ms;
	u64 start, end;

	start = ktime_get_ns();
	res = tzdev_smc_cmd_extended(TZDEV_SMC_SCHEDULE, 0, 0, 0, 0, 0, 0);
	end = ktime_get_ns();
	sk_cpu_mask = tz_iwservice_get_cpu_mask();

	st->entries++;
	st->swd_ns += end - start;
	memcpy(st->ret, res.args, sizeof(st->ret));

	/* A sleep before this entry or an empty mask after it ends a streak */
	if (st->streak && (start - st->last_end > TZ_WORKER_GAP_NS || !sk_cpu_mask)) {
		if (st->reported)
			tzdev_print(0, "tz_worker_thread/%u: streak over after %llu entries in %llu ms, CPUs asked for now %#lx\n",
					cpu, st->streak,
					div_u64(st->last_end - st->streak_start, NSEC_PER_MSEC),
					sk_cpu_mask);
		st->streak = 0;
		st->reported = false;
	}
	st->last_end = end;

	if (!sk_cpu_mask)
		return;

	if (!st->streak++) {
		st->streak_start = start;
		st->streak_swd_ns = 0;
		st->streak_nonzero = 0;
	}
	st->streak_swd_ns += end - start;
	if (res.args[0])
		st->streak_nonzero++;
	if (end - st->streak_start > st->longest_ns) {
		st->longest_ns = end - st->streak_start;
		st->longest_entries = st->streak;
	}

	report_ms = READ_ONCE(tz_worker_report_ms);
	if (report_ms && !st->reported &&
			end - st->streak_start >= (u64)report_ms * NSEC_PER_MSEC) {
		st->reported = true;
		tz_worker_report(cpu, st);
	}
}

static int tz_worker_diag_get(char *buffer, const struct kernel_param *kp)
{
	return tz_worker_diag_print(buffer, PAGE_SIZE);
}

static int tz_worker_diag_set(const char *val, const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops tz_worker_diag_ops = {
	.set = tz_worker_diag_set,
	.get = tz_worker_diag_get,
};

module_param_cb(worker_diag, &tz_worker_diag_ops, NULL, 0400);

static void tz_worker_handler(unsigned int cpu)
{
	int ret;

	for (;;) {
		ret = tz_kthread_pool_wait_for_event(cpu);

		switch (ret) {
		case KTHREAD_SHOULD_STOP:
		case KTHREAD_SHOULD_PARK:
			return;
		case KTHREAD_SHOULD_RUN:
			tzdev_kthread_debug("Enter SWd from kthread on cpu = %u\n", cpu);
			tz_worker_enter_swd(cpu);
			tzdev_kthread_debug("Exit SWd from kthread on cpu = %u\n", cpu);
			continue;
		default:
			BUG();
		}
	}
}

static int tz_kthread_pool_should_run(unsigned int cpu)
{
	(void) cpu;

	/*
	 * Implementing wait_for_event here is prohibited as
	 * this function called with preemption disabled.
	 *
	 * So, always return 1 here to unconditionally call
	 * for tz_worker_handler where kthread will sleep
	 * on waitqueue waiting for request.
	 */
	return 1;
}

static struct smp_hotplug_thread tz_kthread_pool_smp_hotplug = {
	.store = &worker,
	.thread_should_run = tz_kthread_pool_should_run,
	.thread_fn = tz_worker_handler,
	.thread_comm = "tz_worker_thread/%u",
};

static __init int tz_kthread_pool_init(void)
{
	return smpboot_register_percpu_thread(&tz_kthread_pool_smp_hotplug);
}

early_initcall(tz_kthread_pool_init);

void tz_kthread_pool_fini(void)
{
	if (atomic_cmpxchg(&tz_kthread_pool_fini_done, 0, 1))
		return;

	smpboot_unregister_percpu_thread(&tz_kthread_pool_smp_hotplug);
}

void tz_kthread_pool_cmd_send(void)
{
	atomic_inc(&tz_nr_cmds);
	tz_kthread_pool_wake_up_all();
}

void tz_kthread_pool_enter_swd(void)
{
	unsigned long cpu;
	unsigned long sk_cpu_mask;
	unsigned long sk_user_cpu_mask;
	cpumask_t requested_cpu_mask;
	cpumask_t requested_user_cpu_mask;

	sk_cpu_mask = tz_iwservice_get_cpu_mask();
	smp_rmb();
	sk_user_cpu_mask = tz_iwservice_get_user_cpu_mask();

	cpumask_copy(&requested_cpu_mask, to_cpumask(&sk_cpu_mask));
	cpumask_copy(&requested_user_cpu_mask, to_cpumask(&sk_user_cpu_mask));

	cpu = get_cpu();
	if (!sk_user_cpu_mask || cpu_isset(cpu, requested_user_cpu_mask) ||
			cpu_isset(cpu, requested_cpu_mask)) {
		tz_kthread_pool_wake_up_all_but(cpu);

		tzdev_kthread_debug("Enter SWd directly on cpu = %lu\n", cpu);
		tzdev_smc_schedule();
		tzdev_kthread_debug("Exit SWd directly on cpu = %lu\n", cpu);
	} else {
		tz_kthread_pool_cmd_send();
	}
	put_cpu();
}
