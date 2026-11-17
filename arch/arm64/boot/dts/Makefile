# SPDX-License-Identifier: GPL-2.0

# Exynos 3830
dtbo-y += exynos/exynos3830-universal3830_r15.dtbo
dtbo-y += exynos/exynos3830-universal3830_r01.dtbo
dtb-y  += exynos/exynos7-espresso.dtb
dtbo-y += exynos/exynos3830-universal3830_r04.dtbo
dtbo-y += exynos/exynos3830-universal3830_r00.dtbo
dtbo-y += exynos/exynos3830-universal3830_r02.dtbo
dtb-y  += exynos/exynos5433-tm2.dtb
dtb-y  += exynos/exynos5433-tm2e.dtb
dtbo-y += exynos/exynos3830-universal3830_r03.dtbo
dtb-y  += exynos/exynos3830.dtb

# Galaxy A21s
dtbo-y += samsung/a21s/a21s_eur_open_w00_r00.dtbo
dtbo-y += samsung/a21s/a21s_eur_open_w00_r03.dtbo
dtbo-y += samsung/a21s/a21s_eur_open_w00_r01.dtbo
dtbo-y += samsung/a21s/a21s_eur_open_w00_r06.dtbo
dtbo-y += samsung/a21s/a21s_eur_open_w00_r02.dtbo

# Galaxy Tab S6 Lite LTE (gta4xl, Exynos 9611)
dtb-y  += exynos/exynos9610.dtb
dtbo-y += samsung/exynos9611-gta4xl_eur_open_01.dtbo
dtbo-y += samsung/exynos9611-gta4xl_eur_open_03.dtbo
dtbo-y += samsung/exynos9611-gta4xl_eur_open_04.dtbo
dtbo-y += samsung/exynos9611-gta4xl_eur_open_05.dtbo
dtbo-y += samsung/exynos9611-gta4xl_eur_open_08.dtbo
dtbo-y += samsung/exynos9611-gta4xl_eur_open_09.dtbo
# The bootloader of this board has only ever been given trees with both phandle properties
# ("linux,phandle" as well), which the dtc of its 4.14 kernel wrote by default. With this the
# blobs are byte for byte those of the 4.14 build.
DTC_FLAGS_exynos9610 += -H both
DTC_FLAGS_exynos9611-gta4xl_eur_open_01 += -H both
DTC_FLAGS_exynos9611-gta4xl_eur_open_03 += -H both
DTC_FLAGS_exynos9611-gta4xl_eur_open_04 += -H both
DTC_FLAGS_exynos9611-gta4xl_eur_open_05 += -H both
DTC_FLAGS_exynos9611-gta4xl_eur_open_08 += -H both
DTC_FLAGS_exynos9611-gta4xl_eur_open_09 += -H both

targets += dtbs
DTB_LIST  := $(dtb-y) $(dtbo-y)
always    := $(DTB_LIST)

dtbs: $(addprefix $(obj)/, $(DTB_LIST))

clean-files := *.dtb*
