# SPDX-License-Identifier: GPL-2.0
# SM5713 MUIC of gta4xl, from the 4.14 kernel
obj-$(CONFIG_MUIC_SM5713)	+= muic-core.o sm5713-muic.o sm5713-muic-afc.o
ifeq ($(CONFIG_MUIC_SUPPORT_CCIC), y)
obj-$(CONFIG_MUIC_SM5713)	+= sm5713-muic-ccic.o
endif
