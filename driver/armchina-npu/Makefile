# SPDX-License-Identifier: GPL-2.0
#
# In-tree Kbuild file for the ArmChina Zhouyi NPU driver, used when this
# directory is copied to e.g. drivers/misc/armchina-npu (see Kconfig).
#
# Out-of-tree builds (npu.mk / DKMS) do not use this file: they go through
# ../Makefile, which builds aipu.ko for SKY1 + Zhouyi V3 directly.
#
# This file used to be a copy of ../Makefile: in-tree it then ignored
# CONFIG_ARMCHINA_NPU and pointed at armchina-npu/armchina-npu/... paths.

ccflags-y += -I$(src) -I$(src)/include -I$(src)/zhouyi
# string CONFIG_ values are unquoted in include/config/auto.conf
ccflags-y += -DKMD_VERSION='"$(CONFIG_ARMCHINA_NPU_KMD_VERSION)"'

# the sources test these historical names rather than the Kconfig symbols
ccflags-$(CONFIG_ARMCHINA_NPU_SOC_SKY1)        += -DCONFIG_SKY1
ccflags-$(CONFIG_ARMCHINA_NPU_SKY1_DEVFREQ)    += -DCONFIG_ENABLE_DEVFREQ

obj-$(CONFIG_ARMCHINA_NPU) += armchina_npu.o
armchina_npu-y := aipu.o aipu_common.o aipu_io.o aipu_irq.o \
		  aipu_job_manager.o aipu_mm.o aipu_dma_buf.o aipu_priv.o \
		  aipu_tcb.o

include $(src)/zhouyi/Makefile
include $(src)/default/Makefile
include $(src)/r329/Makefile
include $(src)/sky1/Makefile
