/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GTA4XL_DEBUG_H
#define _LINUX_GTA4XL_DEBUG_H

#include <linux/types.h>
/* RAM log, checkpoints and reset handling of gta4xl: arch/arm64/kernel/gta4xl_early_ramlog.c */
#ifdef CONFIG_GTA4XL_RAMLOG
void gta4xl_early_ramlog_init(void);
bool gta4xl_early_ramlog_active(void);
void gta4xl_checkpoint(int n, const char *name);
void gta4xl_next_boot_recovery(void);
void gta4xl_test_boot_ends(void);
void gta4xl_power_off_mark(void);
bool gta4xl_test_boot(void);
void gta4xl_trace(const char *fmt, ...) __printf(1, 2);
#else
static inline void gta4xl_early_ramlog_init(void) { }
static inline bool gta4xl_early_ramlog_active(void) { return false; }
static inline void gta4xl_checkpoint(int n, const char *name) { }
static inline void gta4xl_next_boot_recovery(void) { }
static inline void gta4xl_test_boot_ends(void) { }
static inline void gta4xl_power_off_mark(void) { }
static inline bool gta4xl_test_boot(void) { return false; }
static inline __printf(1, 2) void gta4xl_trace(const char *fmt, ...) { }
#endif
#endif
