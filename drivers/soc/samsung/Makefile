# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_EXYNOS_ASV_ARM)	+= exynos5422-asv.o
obj-$(CONFIG_EXYNOS_CHIPID)	+= exynos_chipid.o
exynos_chipid-y			+= exynos-chipid.o exynos-asv.o

obj-$(CONFIG_EXYNOS_USI)	+= exynos-usi.o

obj-$(CONFIG_EXYNOS_PMU)	+= exynos-pmu.o

obj-$(CONFIG_EXYNOS_PMU_ARM_DRIVERS)	+= exynos3250-pmu.o exynos4-pmu.o \
					exynos5250-pmu.o exynos5420-pmu.o
obj-$(CONFIG_EXYNOS_PM_DOMAINS) += pm_domains.o
obj-$(CONFIG_EXYNOS_REGULATOR_COUPLER) += exynos-regulator-coupler.o

obj-$(CONFIG_SAMSUNG_PM_CHECK)	+= s3c-pm-check.o
obj-$(CONFIG_SAMSUNG_PM_DEBUG)	+= s3c-pm-debug.o

# EXYNOS_BCM
obj-$(CONFIG_EXYNOS_BCM_DBG)    	+= exynos-bcm.o
obj-$(CONFIG_EXYNOS_BCM_DBG_DUMP)       += exynos-bcm_dbg-dump.o
exynos-bcm-y				+= exynos-bcm_dbg.o exynos-bcm_dbg-dt.o
exynos-bcm-$(CONFIG_SOC_S5E9935) 	+= exynos-bcm_dbg-9935.o
exynos-bcm-$(CONFIG_SOC_S5E9945) 	+= exynos-bcm_dbg-9945.o
exynos-bcm-$(CONFIG_SOC_S5E8845) 	+= exynos-bcm_dbg-8845.o

obj-$(CONFIG_SOC_SAMSUNG)       += exynos/
