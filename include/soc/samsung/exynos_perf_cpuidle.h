/*
 * Exynos CPU Performance
 * Jungwook Kim <jwook1.kim@samsung.com>
 */

#ifndef EXYNOS_PERF_CPUIDLE_H
#define EXYNOS_PERF_CPUIDLE_H __FILE__

#ifdef CONFIG_EXYNOS_CPU_PERF
extern void exynos_perf_cpu_idle_enter(int cpu, int index);
extern void exynos_perf_cpu_idle_exit(int cpu, int index, int cancel);
extern void exynos_perf_cpu_idle_register(struct cpuidle_driver *drv);
extern void exynos_perf_cpuidle_start(void);
extern void exynos_perf_cpuidle_stop(void);
#else
struct cpuidle_driver;
static inline void exynos_perf_cpu_idle_enter(int cpu, int index) { }
static inline void exynos_perf_cpu_idle_exit(int cpu, int index, int cancel) { }
static inline void exynos_perf_cpu_idle_register(struct cpuidle_driver *drv) { }
static inline void exynos_perf_cpuidle_start(void) { }
static inline void exynos_perf_cpuidle_stop(void) { }
#endif

#endif
