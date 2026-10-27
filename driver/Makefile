# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree Kbuild file for the ArmChina Zhouyi V3 NPU on CIX SKY1 / CD8180
# (Orange Pi 6 Plus). Driven by ../npu.mk or by DKMS (dkms.conf).
#
# Knobs (command line or environment):
#   BUILD_AIPU_VERSION_KMD           BUILD_ZHOUYI_V3 (only value supported)
#   BUILD_TARGET_PLATFORM_KMD        BUILD_PLATFORM_SKY1 (only value supported)
#   COMPASS_DRV_BTENVAR_KMD_VERSION  version string reported to the UMD
#   BUILD_NPU_DEVFREQ=y|n            DVFS, needs scmi_device_*() (CIX vendor kernel)
#   AIPU_SKIP_IOVA_RESERVE=y|n       skip the "never map" IOVA reservation

BUILD_AIPU_VERSION_KMD          ?= BUILD_ZHOUYI_V3
BUILD_TARGET_PLATFORM_KMD       ?= BUILD_PLATFORM_SKY1
COMPASS_DRV_BTENVAR_KMD_VERSION ?= 5.11.0
BUILD_NPU_DEVFREQ               ?= n
AIPU_SKIP_IOVA_RESERVE          ?= n

ifneq ($(KERNELRELEASE),)
# ------------------------------------------------------------------ Kbuild
ifneq ($(BUILD_AIPU_VERSION_KMD),BUILD_ZHOUYI_V3)
$(error aipu: only BUILD_AIPU_VERSION_KMD=BUILD_ZHOUYI_V3 is supported by this Kbuild)
endif
ifneq ($(BUILD_TARGET_PLATFORM_KMD),BUILD_PLATFORM_SKY1)
$(error aipu: only BUILD_TARGET_PLATFORM_KMD=BUILD_PLATFORM_SKY1 is supported by this Kbuild)
endif

AIPU_DIR := $(src)/armchina-npu

ccflags-y += -DCONFIG_SKY1 \
             -DCONFIG_ARMCHINA_NPU_ARCH_V3 \
             -D$(BUILD_AIPU_VERSION_KMD) \
             -D$(BUILD_TARGET_PLATFORM_KMD) \
             -DKMD_VERSION=\"$(COMPASS_DRV_BTENVAR_KMD_VERSION)\" \
             -I$(AIPU_DIR) \
             -I$(AIPU_DIR)/include \
             -I$(AIPU_DIR)/zhouyi

ifeq ($(BUILD_NPU_DEVFREQ),y)
ccflags-y += -DCONFIG_ENABLE_DEVFREQ
endif
ifeq ($(AIPU_SKIP_IOVA_RESERVE),y)
ccflags-y += -DAIPU_SKIP_IOVA_RESERVE
endif

# Vendor code is not prototype-clean; keep CONFIG_WERROR kernels building.
ccflags-y += -Wno-missing-prototypes -Wno-unused-label -Wno-unused-function

obj-m := aipu.o
aipu-y := armchina-npu/sky1/sky1.o \
          armchina-npu/aipu.o \
          armchina-npu/aipu_common.o \
          armchina-npu/aipu_io.o \
          armchina-npu/aipu_irq.o \
          armchina-npu/aipu_job_manager.o \
          armchina-npu/aipu_mm.o \
          armchina-npu/aipu_dma_buf.o \
          armchina-npu/aipu_priv.o \
          armchina-npu/aipu_tcb.o \
          armchina-npu/zhouyi/zhouyi.o \
          armchina-npu/zhouyi/v3.o \
          armchina-npu/zhouyi/v3_priv.o

else
# ------------------------------------------------------------------ wrapper
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

KBUILD_ARGS := -C $(KDIR) M=$(PWD) \
	BUILD_AIPU_VERSION_KMD=$(BUILD_AIPU_VERSION_KMD) \
	BUILD_TARGET_PLATFORM_KMD=$(BUILD_TARGET_PLATFORM_KMD) \
	COMPASS_DRV_BTENVAR_KMD_VERSION=$(COMPASS_DRV_BTENVAR_KMD_VERSION) \
	BUILD_NPU_DEVFREQ=$(BUILD_NPU_DEVFREQ) \
	AIPU_SKIP_IOVA_RESERVE=$(AIPU_SKIP_IOVA_RESERVE)

.PHONY: all modules modules_install clean
all modules:
	$(MAKE) $(KBUILD_ARGS) modules

modules_install:
	$(MAKE) $(KBUILD_ARGS) modules_install

clean:
	$(MAKE) $(KBUILD_ARGS) clean
endif
