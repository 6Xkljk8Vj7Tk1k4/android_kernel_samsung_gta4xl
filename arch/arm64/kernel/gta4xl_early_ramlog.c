// SPDX-License-Identifier: GPL-2.0
/*
 * RAM log, checkpoints and reset handling for gta4xl, a board without an accessible UART.
 *
 * Log. From setup_arch() on, every printk line is also written, uncached, into the pstore
 * console zone of the debug-snapshot "log_pstore" region, in the persistent_ram format, so that
 * a kernel with a working ramoops (the 4.14 one in the recovery partition) shows it as
 * /sys/fs/pstore/console-ramoops-0. Layout (lib/debug-snapshot-pstore.c): a dump zone of 512K,
 * then the console zone (512K). Three early_ioremap slots (256K each) cover the last page of
 * the dump zone, which holds this file's own words, and the console zone; at core_initcall time
 * one regular mapping replaces them. The zone is a ring. A start that finds the ring still
 * holding what this kernel wrote last continues behind that text, and at core_initcall time
 * copies what is left of it to /proc/gta4xl_last_kmsg (root): the end of the start before,
 * however it ended. After another kernel has run, the ring starts empty.
 * While this console owns the zone, lib/debug-snapshot-pstore.c does not register ramoops.
 *
 * Resets. The bootloader reads PMU INFORM3 after a warm reset (see drivers/samsung/
 * sec_reboot.c for the values). This file sets "next start: recovery" first thing, so that a
 * kernel dying in its initcalls ends in recovery, and takes it back at late_initcall_sync by
 * putting the words the bootloader had left. A run-state word next to the log tells the next
 * start how this one ended. For the first UP_AFTER seconds a panic resets to recovery, and a
 * start that finds its predecessor unfinished in that phase (watchdog, forced reset by keys)
 * goes to recovery once, log kept: a kernel that cannot come up does not loop. After that a
 * panic restarts the system the way this board's 4.14 kernel does (upload magic cleared, the
 * upload cause in INFORM3; /proc/reset_reason says KPON), at once and from here, and a start
 * after an unfinished predecessor just goes on.
 *
 * Checkpoints. gta4xl_checkpoint(n, name) writes "GTA4XL-CP n name" into the log.
 *
 * Test boots. gta4xl_test on the kernel command line (the bootloader ignores the one in the
 * boot image: CONFIG_CMDLINE, see arch/arm64/configs/gta4xl_test.config) makes this a kernel
 * to try from the boot partition with the recovery as the way back. Every way such a boot can
 * end leads to recovery:
 *  - gta4xl_reset_after=SECONDS timer, gta4xl_cp=N (reset at checkpoint N), the reboot path;
 *  - panic, at any time;
 *  - hard hang: the SoC watchdog, started by s3c2410_wdt.c with 45 s and kicked only by a
 *    kernel timer here (gta4xl_hang=N hangs at checkpoint N to try that);
 *  - forced reset by keys (a pin reset: the bootloader forgets the mark and starts this kernel
 *    again): the next start sees the run-state word and goes to recovery.
 * A test boot holds a wakeup source, so it never suspends; gta4xl_nosuspend does the same for
 * a regular boot.
 *
 * Addresses are those of the 9610 DTS: log_pstore = <0xf9d10000 0x200000>, PMU at 0x11860000.
 * The kernel's own debug-snapshot-def.h carries another SoC's layout.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/memblock.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/sched/clock.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/bitops.h>
#include <linux/notifier.h>
#include <linux/irqflags.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/sec_debug.h>
#include <linux/gta4xl_debug.h>
#include <soc/samsung/exynos-debug.h>
#include <asm/early_ioremap.h>
#include <linux/smp.h>

#define RAMLOG_PHYS	(0xf9d10000UL + SZ_512K)	/* console zone */
#define RAMLOG_ZONE	SZ_512K
#define RAMLOG_PRE	SZ_4K		/* mapped in front of the zone: end of the dump zone */
#define RAMLOG_WIN	SZ_256K		/* one early_ioremap slot */
#define RAMLOG_NWIN	3		/* slots that cover RAMLOG_PRE + RAMLOG_ZONE */
#define RAMLOG_SIG	0x43474244	/* PERSISTENT_RAM_SIG, "DBGC" */
#define RAMLOG_HDR	12		/* u32 sig, start, size */
/* offsets from the zone start; the harness's words sit just in front of it */
#define RAMLOG_STATE	(-8)		/* run-state word */
#define RAMLOG_COPY	(-16)		/* copy of the header's start and size */
#define RAMLOG_ROOM	(RAMLOG_ZONE - RAMLOG_HDR)

/* Far apart bit-wise: a forced reset flips some RAM bits, the comparison tolerates a few. */
#define STATE_RUNNING	0xa5a5a5a55a5a5a5aULL	/* started */
#define STATE_UP	0x5a5a5a5aa5a5a5a5ULL	/* has run for UP_AFTER seconds */
#define STATE_DONE	0x0f0f0f0ff0f0f0f0ULL	/* ended by itself: restart, power-off, test reset */
#define STATE_PANIC	0xc33cc33c3cc33cc3ULL	/* panic: restarted (once up) or went to recovery */
#define STATE_BAILED	0x33cc33cccc33cc33ULL	/* found its predecessor unfinished: went to recovery */
#define UP_AFTER	120			/* seconds */

#define PMU_PHYS	0x11860000UL
#define PMU_SWRESET	0x0400
#define PMU_INFORM3	0x080c
#define PMU_SYSIP_DAT0	0x0810
/* INFORM3 as this board's bootloader reads it (sec_reboot.c, sec_debug_base.c) */
#define INFORM3_UNKNOWN		0x12345670	/* SEC_RESET_REASON_UNKNOWN: a plain restart */
#define INFORM3_RECOVERY	0x12345674	/* SEC_RESET_REASON_RECOVERY */
#define INFORM3_PANIC		0x000000c8	/* UPLOAD_CAUSE_KERNEL_PANIC */

static void __iomem *ramlog;		/* start of the console zone */
static void __iomem *ramlog_win[RAMLOG_NWIN];	/* early mappings, from RAMLOG_PRE in front of it */
static void __iomem *pmu;
static u32 ramlog_pos;
static u32 ramlog_mine;		/* bytes this start has written, RAMLOG_ROOM at most */
static u32 ramlog_prev;		/* bytes the ring held when this start continued it */
static u32 boot_inform3, boot_sysip;	/* the PMU words as the bootloader left them */
static const char *prev_end = "unknown";	/* how the 4.19 start before this one ended */
static bool is_up;		/* UP_AFTER seconds have passed */
static char *last_log;		/* /proc/gta4xl_last_kmsg */
static size_t last_log_len;
static int reset_at = -1;
static int hang_at = -1;
static int reset_after;
static bool test_boot;		/* gta4xl_test: every end of this boot leads to recovery */
static bool nosuspend;		/* gta4xl_nosuspend: hold a wakeup source in a regular boot too */

static bool ramlog_wrapped;
static DEFINE_RAW_SPINLOCK(ramlog_lock);	/* console writes and gta4xl_trace() callers */

/* Where ring position pos is mapped, and how many bytes follow it there in one piece. */
static void __iomem *ramlog_at(u32 pos, u32 *run)
{
	u32 off;

	*run = RAMLOG_ROOM - pos;
	if (!ramlog_win[0])
		return ramlog + RAMLOG_HDR + pos;
	off = RAMLOG_PRE + RAMLOG_HDR + pos;		/* from the start of the first window */
	*run = min_t(u32, *run, RAMLOG_WIN - off % RAMLOG_WIN);
	return ramlog_win[off / RAMLOG_WIN] + off % RAMLOG_WIN;
}

/* Ring: when the zone is full, the oldest text is overwritten. */
static void ramlog_put(const char *s, unsigned int n)
{
	unsigned long flags;

	if (!ramlog)
		return;
	raw_spin_lock_irqsave(&ramlog_lock, flags);
	while (n) {
		void __iomem *to;
		u32 run, c;

		to = ramlog_at(ramlog_pos, &run);
		c = min_t(u32, n, run);
		memcpy_toio(to, s, c);
		ramlog_pos += c;
		ramlog_mine = min_t(u32, ramlog_mine + c, RAMLOG_ROOM);
		s += c;
		n -= c;
		if (ramlog_pos == RAMLOG_ROOM) {
			ramlog_pos = 0;
			ramlog_wrapped = true;
		}
	}
	writel(ramlog_pos, ramlog + 4);					/* start: next write */
	writel(ramlog_wrapped ? RAMLOG_ROOM : ramlog_pos, ramlog + 8);	/* size: valid bytes */
	/* the 4.14 kernels rewrite the header when they boot, not this copy: see below */
	writel(ramlog_pos, ramlog + RAMLOG_COPY);
	writel(ramlog_wrapped ? RAMLOG_ROOM : ramlog_pos, ramlog + RAMLOG_COPY + 4);
	raw_spin_unlock_irqrestore(&ramlog_lock, flags);
}

/*
 * A line straight into the RAM log, not through printk: for paths where the next instruction
 * may be the last one executed (power-mode entry), so the line must be in RAM before it.
 */
void gta4xl_trace(const char *fmt, ...)
{
	char line[120];
	va_list ap;
	int len;

	if (!ramlog)
		return;
	len = scnprintf(line, sizeof(line), "GTA4XL-T %llu c%d ",
			(unsigned long long)(local_clock() / 1000), raw_smp_processor_id());
	va_start(ap, fmt);
	len += vscnprintf(line + len, sizeof(line) - len, fmt, ap);
	va_end(ap);
	ramlog_put(line, len);
}

static void ramlog_write(struct console *con, const char *s, unsigned int n)
{
	ramlog_put(s, n);
}

static struct console ramlog_console = {
	.name	= "ramlog",
	.write	= ramlog_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

/*
 * "Next start: recovery", the way the 4.14 kernel leaves it for `reboot recovery`: the
 * bootloader honours these PMU registers across warm resets until a kernel rewrites them. A pin
 * reset (Power+VolDown held) clears them. The test kernel sits in the boot partition and the
 * stock recovery is the way back, so set this as early as possible.
 */
void gta4xl_next_boot_recovery(void)
{
	if (!pmu)
		return;
	writel(0xff, pmu + PMU_SYSIP_DAT0);		/* REBOOT_MODE_RECOVERY */
	writel(INFORM3_RECOVERY, pmu + PMU_INFORM3);
}

static void gta4xl_set_state(u64 state)
{
	if (ramlog)
		writeq(state, ramlog + RAMLOG_STATE);
}

static bool gta4xl_state_is(u64 state)
{
	return ramlog && hweight64(readq(ramlog + RAMLOG_STATE) ^ state) <= 12;
}

/*
 * True when the zone still holds what this kernel wrote last, i.e. no 4.14 kernel (recovery,
 * Android) has booted since: those restart the ring and move the header away from our copy.
 */
static bool gta4xl_log_is_ours(void)
{
	return hweight32(readl(ramlog + 4) ^ readl(ramlog + RAMLOG_COPY)) +
	       hweight32(readl(ramlog + 8) ^ readl(ramlog + RAMLOG_COPY + 4)) <= 4;
}

static void gta4xl_reset(u64 state)
{
	if (!pmu)
		return;
	gta4xl_set_state(state);
	gta4xl_next_boot_recovery();
	writel(0x1, pmu + PMU_SWRESET);
	for (;;)
		cpu_relax();
}

/*
 * After a panic, once this boot was up: start again, as the 4.14 kernel does. This is
 * what sec_reboot() leaves for a "panic" restart there: no upload magic at the start of RAM,
 * the upload cause in INFORM3. Done here and at once, like the reset to recovery: the regular
 * panic path of this tree (debug-snapshot, ACPM) has never run on this board.
 */
static void gta4xl_restart_after_panic(void)
{
	gta4xl_set_state(STATE_PANIC);
#ifdef CONFIG_SEC_DEBUG
	secdbg_base_clear_magic_rambase();
#endif
	writel(0x00, pmu + PMU_SYSIP_DAT0);		/* REBOOT_MODE_NORMAL */
	writel(INFORM3_PANIC, pmu + PMU_INFORM3);
	writel(0x1, pmu + PMU_SWRESET);
	for (;;)
		cpu_relax();
}

/*
 * The regular reboot path (exynos-reboot-9610.c), right before its SWRESET. A test boot ends
 * in recovery whatever was asked for; a regular boot only notes that it ended by itself.
 */
void gta4xl_test_boot_ends(void)
{
	gta4xl_set_state(STATE_DONE);
	if (test_boot)
		gta4xl_next_boot_recovery();
}

/* Power off (sec_reboot.c): RAM can outlive a short power cycle, the run-state word with it. */
void gta4xl_power_off_mark(void)
{
	gta4xl_set_state(STATE_DONE);
}

/* True for a test boot: reset timer, own watchdog, never suspends, every restart -> recovery. */
bool gta4xl_test_boot(void)
{
	return ramlog != NULL && test_boot;
}

static struct timer_list reset_timer;
static bool hanging;

void gta4xl_checkpoint(int n, const char *name)
{
	char line[64];
	int len;

	len = scnprintf(line, sizeof(line), "GTA4XL-CP %d %s%s\n", n, name,
			test_boot && n == reset_at ? " -> reset" : "");
	ramlog_put(line, len);
	pr_info("GTA4XL-CP %d %s\n", n, name);
	if (!test_boot)
		return;
	if (n == reset_at)
		gta4xl_reset(STATE_DONE);
	if (n == hang_at) {
		/* a deliberate hard hang: only the watchdog or the keys end this boot */
		hanging = true;
		local_irq_disable();
		for (;;)
			cpu_relax();
	}
}

static int __init gta4xl_cp_setup(char *str)
{
	return kstrtoint(str, 0, &reset_at);
}
early_param("gta4xl_cp", gta4xl_cp_setup);

static int __init gta4xl_hang_setup(char *str)
{
	return kstrtoint(str, 0, &hang_at);
}
early_param("gta4xl_hang", gta4xl_hang_setup);

/*
 * gta4xl_reset_after=SECONDS: a test boot resets to recovery that long after the core
 * initcalls, so that it ends by itself as long as timers still run.
 */
static int __init gta4xl_reset_after_setup(char *str)
{
	return kstrtoint(str, 0, &reset_after);
}
early_param("gta4xl_reset_after", gta4xl_reset_after_setup);

static int __init gta4xl_test_setup(char *str)
{
	test_boot = true;
	return 0;
}
early_param("gta4xl_test", gta4xl_test_setup);

static int __init gta4xl_nosuspend_setup(char *str)
{
	nosuspend = true;
	return 0;
}
early_param("gta4xl_nosuspend", gta4xl_nosuspend_setup);

static void gta4xl_reset_timer_fn(struct timer_list *t)
{
	if (hanging)
		return;
	gta4xl_checkpoint(99, "gta4xl_reset_after expired");
	gta4xl_reset(STATE_DONE);
}

/*
 * /sys/module/gta4xl_early_ramlog/parameters/reset_after: seconds from now to the reset to
 * recovery, so that an adb session can be made longer (or ended) without another image.
 * It only moves an armed timer: a test boot always ends by itself.
 */
static int gta4xl_reset_after_set(const char *val, const struct kernel_param *kp)
{
	int sec, ret = kstrtoint(val, 0, &sec);

	if (ret)
		return ret;
	if (sec < 1 || sec > 86400 || !timer_pending(&reset_timer))
		return -EINVAL;
	reset_after = sec;
	mod_timer(&reset_timer, jiffies + sec * HZ);
	pr_info("GTA4XL: reset to recovery in %d s (set at run time)\n", sec);
	return 0;
}

static const struct kernel_param_ops gta4xl_reset_after_ops = {
	.set	= gta4xl_reset_after_set,
	.get	= param_get_int,
};
module_param_cb(reset_after, &gta4xl_reset_after_ops, &reset_after, 0600);

static int gta4xl_panic(struct notifier_block *nb, unsigned long l, void *p)
{
	static const char msg[] = "GTA4XL: panic, reset to recovery\n";
	static const char again[] = "GTA4XL: panic, restart\n";

	if (!test_boot && is_up && pmu) {
		ramlog_put(again, sizeof(again) - 1);
		gta4xl_restart_after_panic();
	}
	ramlog_put(msg, sizeof(msg) - 1);
	gta4xl_reset(STATE_PANIC);
	return NOTIFY_DONE;
}

/* From here on a panic or an unfinished end no longer leads to recovery. */
static struct timer_list up_timer;

static void gta4xl_up_fn(struct timer_list *t)
{
	is_up = true;
	if (gta4xl_state_is(STATE_RUNNING))
		gta4xl_set_state(STATE_UP);
}

/*
 * /proc/gta4xl_last_kmsg: what the ring held before this start, less what this start has
 * overwritten since (the oldest text goes first, the end of the older log last).
 */
static ssize_t gta4xl_last_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	return simple_read_from_buffer(buf, len, pos, last_log, last_log_len);
}

static const struct file_operations gta4xl_last_fops = {
	.read	= gta4xl_last_read,
	.llseek	= default_llseek,
};

static void __init gta4xl_keep_last_log(void)
{
	struct proc_dir_entry *e;
	unsigned long flags;
	u32 old, from, n;
	char *buf;

	if (!ramlog_prev)
		return;
	buf = vmalloc(ramlog_prev);
	if (!buf)
		return;
	raw_spin_lock_irqsave(&ramlog_lock, flags);
	old = (ramlog_wrapped ? RAMLOG_ROOM : ramlog_pos) - ramlog_mine;
	old = min(old, ramlog_prev);
	from = ramlog_wrapped ? ramlog_pos : 0;		/* the oldest byte in the ring */
	n = min_t(u32, old, RAMLOG_ROOM - from);
	memcpy_fromio(buf, ramlog + RAMLOG_HDR + from, n);
	memcpy_fromio(buf + n, ramlog + RAMLOG_HDR, old - n);
	raw_spin_unlock_irqrestore(&ramlog_lock, flags);
	if (!old) {
		vfree(buf);
		return;
	}
	last_log = buf;
	last_log_len = old;
	e = proc_create("gta4xl_last_kmsg", 0400, NULL, &gta4xl_last_fops);
	if (e)
		proc_set_size(e, old);
}

/*
 * Kicks the watchdog every 10 s so that a test boot may last as long as gta4xl_reset_after
 * says (an adb session on the recovery ramdisk, for instance). When the CPU serving timers
 * hangs, the kicks stop and the watchdog resets the SoC.
 */
static struct timer_list wdt_kick_timer;

static void gta4xl_wdt_kick_fn(struct timer_list *t)
{
	if (hanging)
		return;
	s3c2410wdt_keepalive_emergency(false, 0);
	mod_timer(&wdt_kick_timer, jiffies + 10 * HZ);
}

/* Like persistent_ram: RAM that has struct pages is mapped with vmap(), anything else ioremap(). */
static void __iomem *gta4xl_map(phys_addr_t phys, size_t size)
{
	unsigned int i, n = size >> PAGE_SHIFT;
	struct page **pages;
	void *va;

	if (!pfn_valid(__phys_to_pfn(phys)))
		return ioremap(phys, size);

	pages = kmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;
	for (i = 0; i < n; i++)
		pages[i] = pfn_to_page(__phys_to_pfn(phys) + i);
	va = vmap(pages, n, VM_MAP, pgprot_noncached(PAGE_KERNEL));
	kfree(pages);
	return (void __iomem *)va;
}

/*
 * Test boots never suspend: the suspend and SICD paths (exynos-pm.c, exynos-powermode.c) are
 * still this tree's, written for another SoC's PMU, and a hang in suspend has no watchdog.
 */
static struct wakeup_source gta4xl_test_ws;

static struct notifier_block gta4xl_panic_nb = {
	.notifier_call	= gta4xl_panic,
	.priority	= INT_MAX,
};

static int __init gta4xl_reset_timer_init(void)
{
	if (!test_boot)
		reset_after = 0;
	if (reset_after > 0 && pmu) {
		timer_setup(&reset_timer, gta4xl_reset_timer_fn, 0);
		mod_timer(&reset_timer, jiffies + reset_after * HZ);
		pr_info("GTA4XL: reset to recovery in %d s\n", reset_after);
	}
	if (ramlog && pmu) {
		void __iomem *full = gta4xl_map(RAMLOG_PHYS - RAMLOG_PRE, RAMLOG_PRE + RAMLOG_ZONE);
		void __iomem *pmu_full = ioremap(PMU_PHYS, SZ_4K);

		/* from the early mappings to regular ones */
		if (full && pmu_full) {
			void __iomem *old_win[RAMLOG_NWIN], *old_pmu = pmu;
			unsigned long flags;
			int i;

			raw_spin_lock_irqsave(&ramlog_lock, flags);
			ramlog = full + RAMLOG_PRE;
			pmu = pmu_full;
			for (i = 0; i < RAMLOG_NWIN; i++) {
				old_win[i] = ramlog_win[i];
				ramlog_win[i] = NULL;
			}
			raw_spin_unlock_irqrestore(&ramlog_lock, flags);
			for (i = 0; i < RAMLOG_NWIN; i++)
				early_iounmap(old_win[i], RAMLOG_WIN);
			early_iounmap(old_pmu, SZ_4K);
			gta4xl_keep_last_log();
		}
		atomic_notifier_chain_register(&panic_notifier_list, &gta4xl_panic_nb);
		if (test_boot || nosuspend) {
			wakeup_source_init(&gta4xl_test_ws, "gta4xl_test_boot");
			__pm_stay_awake(&gta4xl_test_ws);
		}
		if (test_boot) {
			timer_setup(&wdt_kick_timer, gta4xl_wdt_kick_fn, 0);
			mod_timer(&wdt_kick_timer, jiffies + 10 * HZ);
		} else {
			timer_setup(&up_timer, gta4xl_up_fn, 0);
			mod_timer(&up_timer, jiffies + UP_AFTER * HZ);
		}
		if (test_boot)
			pr_info("GTA4XL: test boot, every end of it leads to recovery\n");
		else if (nosuspend)
			pr_info("GTA4XL: suspend blocked (gta4xl_nosuspend)\n");
		pr_info("GTA4XL: the start before this one ended: %s; older log kept: %zu bytes; the bootloader left INFORM3 %08x, SYSIP_DAT0 %08x\n",
			prev_end, last_log_len, boot_inform3, boot_sysip);
	}
	return 0;
}
core_initcall(gta4xl_reset_timer_init);

/*
 * The initcalls are through, so from here on a reset that nobody announced (watchdog) shall
 * start this kernel again. The early mark is taken back by putting the words the bootloader
 * left, which is what the 4.14 kernel runs with; a reason sec_reboot or sec_debug has put into
 * INFORM3 in the meantime is theirs. A test boot keeps the mark.
 */
static int __init gta4xl_clear_early_mark(void)
{
	if (test_boot || !pmu)
		return 0;
	writel(boot_sysip, pmu + PMU_SYSIP_DAT0);
	if (readl(pmu + PMU_INFORM3) == INFORM3_RECOVERY)
		writel(boot_inform3 == INFORM3_RECOVERY ? INFORM3_UNKNOWN : boot_inform3,
		       pmu + PMU_INFORM3);
	pr_info("GTA4XL: through the initcalls, a reset starts this kernel again\n");
	return 0;
}
late_initcall_sync(gta4xl_clear_early_mark);

void __init gta4xl_early_ramlog_init(void)
{
	static const char mark[] __initconst = "=== GTA4XL-EARLY 4.19 log start ===\n";
	static const char bail[] __initconst =
		"\n=== GTA4XL: the previous start did not end by itself before it was up (hang, "
		"forced reset?): its log is above, going to recovery ===\n";
	u32 start, size;
	bool sane, ours;
	int i;

	/*
	 * The zone must never reach the page allocator, whatever the debug-snapshot code decides
	 * about its "log_pstore" item: this runs before arm64_memblock_init().
	 */
	memblock_reserve(RAMLOG_PHYS - RAMLOG_PRE, RAMLOG_PRE + RAMLOG_ZONE);

	pmu = early_ioremap(PMU_PHYS, SZ_4K);
	if (pmu) {
		boot_inform3 = readl(pmu + PMU_INFORM3);
		boot_sysip = readl(pmu + PMU_SYSIP_DAT0);
	}
	gta4xl_next_boot_recovery();
	for (i = 0; i < RAMLOG_NWIN; i++) {
		ramlog_win[i] = early_ioremap(RAMLOG_PHYS - RAMLOG_PRE + i * RAMLOG_WIN, RAMLOG_WIN);
		if (!ramlog_win[i]) {
			while (i--)
				early_iounmap(ramlog_win[i], RAMLOG_WIN);
			ramlog_win[0] = NULL;
			return;
		}
	}
	ramlog = ramlog_win[0] + RAMLOG_PRE;

	/* a ring as persistent_ram keeps it: not yet full (start == size) or full */
	start = readl(ramlog + 4);
	size = readl(ramlog + 8);
	sane = readl(ramlog) == RAMLOG_SIG && start < RAMLOG_ROOM &&
	       (size == start || size == RAMLOG_ROOM);
	ours = gta4xl_log_is_ours();

	if (gta4xl_state_is(STATE_RUNNING) && ours) {
		/*
		 * That start did not get as far as "up". Keep its log and add a line; when the
		 * header has lost bits in the reset, offer the whole zone instead.
		 */
		if (sane) {
			ramlog_pos = start;
			ramlog_wrapped = size == RAMLOG_ROOM;
			ramlog_put(bail, sizeof(bail) - 1);
		} else {
			writel(RAMLOG_SIG, ramlog);
			writel(0, ramlog + 4);
			writel(RAMLOG_ROOM, ramlog + 8);
		}
		gta4xl_reset(STATE_BAILED);
	}
	if (gta4xl_state_is(STATE_DONE))
		prev_end = "by itself (restart, power-off, test reset)";
	else if (gta4xl_state_is(STATE_PANIC))
		prev_end = "panic";
	else if (gta4xl_state_is(STATE_UP))
		prev_end = "not by itself, after it was up (watchdog, forced reset, power loss)";
	else if (gta4xl_state_is(STATE_BAILED))
		prev_end = "sent to recovery: its predecessor had not come up";
	else if (gta4xl_state_is(STATE_RUNNING))
		prev_end = "not by itself, before it was up; another kernel has run since";

	if (sane && ours) {
		/* go on behind the text of the 4.19 start before this one */
		ramlog_pos = start;
		ramlog_wrapped = size == RAMLOG_ROOM;
		ramlog_prev = size;
	} else {
		writel(RAMLOG_SIG, ramlog);
		writel(0, ramlog + 4);
		writel(0, ramlog + 8);
	}
	gta4xl_set_state(STATE_RUNNING);
	ramlog_put(mark, sizeof(mark) - 1);
	register_console(&ramlog_console);
}

/*
 * True while this console owns the pstore console zone. lib/debug-snapshot-pstore.c then does
 * not register ramoops: two writers in one zone would corrupt the log (and ramoops's probe has
 * not been seen working on this board yet).
 */
bool gta4xl_early_ramlog_active(void)
{
	return ramlog != NULL;
}
