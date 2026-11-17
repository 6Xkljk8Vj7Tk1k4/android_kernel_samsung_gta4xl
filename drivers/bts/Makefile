#
# Makefile for BTS device driver
#

ifdef CONFIG_EXYNOS9610_BTS
obj-$(CONFIG_EXYNOS9610_BTS)	+= cal_bts9610.o bts-exynos9610.o
else
obj-$(CONFIG_EXYNOS_BTS)	+= exynos-bts.o
endif
obj-$(CONFIG_EXYNOS9630_BTS)	+= exynos-btsops9630.o
obj-$(CONFIG_EXYNOS3830_BTS)	+= exynos-btsops3830.o
