# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V1),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_soc_3.o abox_cmpnt_3.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V2),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_soc_3.o abox_cmpnt_3.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V3),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_soc_3.o abox_cmpnt_3.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V3_1),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_soc_31.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_4.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4_01),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_401.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4_02),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_402.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4_2),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_42.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4_22),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_422.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4_23),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_423.o abox_soc.o abox_cmpnt.o
else ifeq ($(CONFIG_SND_SOC_SAMSUNG_ABOX_V4_3),y)
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) +=  abox_soc_43.o abox_soc.o abox_cmpnt.o
else
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_soc.o abox_cmpnt.o
endif

snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX_UDMA) += abox_udma.o
snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX_PCIE) += abox_pci.o

snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX_ATUNE) += abox_atune.o

snd-soc-samsung-abox-gic-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_gic.o abox_util.o

snd-soc-samsung-abox-sync-$(CONFIG_SEC_SND_SYNCHRONIZED_IPC) += abox_synchronized_ipc.o
snd-soc-samsung-abox-sync-ti-$(CONFIG_SEC_SND_SYNCHRONIZED_IPC_TI) += abox_synchronized_ipc_ti.o

snd-soc-samsung-abox-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += abox_util.o \
	abox_dbg.o abox_dump.o abox_log.o abox.o abox_core.o abox_rdma.o\
	abox_wdma.o abox_if.o abox_effect.o abox_vss.o abox_failsafe.o \
	abox_vdma.o abox_msg.o abox_ipc.o abox_qos.o abox_ion.o abox_tplg.o \
	abox_dma.o abox_shm.o abox_proc.o abox_oem.o \
	abox_memlog.o

obj-$(CONFIG_SND_SOC_SAMSUNG_ABOX) += snd-soc-samsung-abox-gic.o snd-soc-samsung-abox.o

obj-$(CONFIG_SEC_SND_SYNCHRONIZED_IPC) += snd-soc-samsung-abox-sync.o
obj-$(CONFIG_SEC_SND_SYNCHRONIZED_IPC_TI) += snd-soc-samsung-abox-sync-ti.o

obj-$(CONFIG_SEC_SND_ADAPTATION) += abox_adaptation.o
