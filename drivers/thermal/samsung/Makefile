# SPDX-License-Identifier: GPL-2.0-only
#
# Samsung thermal specific Makefile
#
obj-$(CONFIG_EXYNOS_THERMAL)			+= exynos_thermal.o
exynos_thermal-y				:= exynos_tmu.o

obj-$(CONFIG_EXYNOS_THERMAL_V2)			+= exynos_thermal_v2.o
exynos_thermal_v2-y				+= exynos_tmu_v2.o
exynos_thermal_v2-y				+= exynos_acpm_tmu.o
exynos_thermal_v2-$(CONFIG_EXYNOS_CPU_THERMAL)	+= cpu_cooling.o
exynos_thermal_v2-$(CONFIG_ISP_THERMAL)		+= isp_cooling.o
exynos_thermal_v2-$(CONFIG_DEV_THERMAL)		+= dev_cooling.o

obj-$(CONFIG_GPU_THERMAL)			+= gpu_cooling.o

# Exynos AMB Control
obj-$(CONFIG_EXYNOS_AMB_CONTROL)		+= exynos_amb_control.o
