# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(src)			# needed for trace events

obj-$(CONFIG_ASHMEM)			+= ashmem.o

# Switch Class Driver
obj-y += switch/

# freecess
obj-$(CONFIG_SAMSUNG_FREECESS)		+= freecess.o
obj-$(CONFIG_SAMSUNG_FREECESS)		+= freecess_pkg.o
obj-$(CONFIG_SAMSUNG_LMKD_DEBUG)	+= lmkd_debug.o
