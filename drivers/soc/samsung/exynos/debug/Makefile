#
#  Makefile for Exynos-Debug
#

obj-$(CONFIG_DEBUG_SNAPSHOT) 		:= dss.o
dss-y 					+= debug-snapshot.o \
					debug-snapshot-log.o\
					debug-snapshot-utils.o \
					debug-snapshot-dpm.o \
					debug-snapshot-pmsg.o \
					debug-snapshot-builtin-wdt.o \
					cache.o

obj-$(CONFIG_DEBUG_SNAPSHOT_DEBUG_KINFO) += debug-snapshot-debug-kinfo.o
obj-$(CONFIG_DEBUG_SNAPSHOT_SFRDUMP)	+= debug-snapshot-sfrdump.o
obj-$(CONFIG_DEBUG_SNAPSHOT_QUICKDUMP)	+= debug-snapshot-qd.o

obj-$(CONFIG_EXYNOS_DEBUG_TEST)         += exynos-debug-test.o

obj-$(CONFIG_EXYNOS_CORESIGHT)		+= exynos-coresight.o

obj-$(CONFIG_EXYNOS_ECC_HANDLER)	+= exynos-ecc-handler.o

obj-$(CONFIG_EXYNOS_HARDLOCKUP_WATCHDOG) += hardlockup-watchdog.o

obj-$(CONFIG_EXYNOS_ITMON)		+= exynos-itmon.o
obj-$(CONFIG_EXYNOS_ITMON_V2)		+= exynos-itmon-v2.o

obj-$(CONFIG_EXYNOS_ADV_TRACER) 	+= exynos-adv-tracer.o

obj-$(CONFIG_EXYNOS_ADV_TRACER_S2D)	+= exynos-adv-tracer-s2d.o

obj-$(CONFIG_EXYNOS_EHLD)		+= ehld.o
ehld-y					:= exynos-ehld.o \
					   exynos-ehld-ipc.o

obj-$(CONFIG_EXYNOS_SSLD)		+= exynos-ssld.o
obj-$(CONFIG_EXYNOS_GETCPUSTATE)	+= exynos-getcpustate.o

obj-$(CONFIG_EXYNOS_PED)		+= exynos-ped.o
