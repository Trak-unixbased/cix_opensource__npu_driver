#  Copyright 2024 Cix Technology Group Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Description: build the NPU OOT kernel module (aipu.ko) for CIX SKY1 /
#              CD8180 boards (Orange Pi 6 Plus) running a regular Linux
#              distribution (Ubuntu 26.04 LTS aarch64, kernel 7.0).
# Author: Andy Tian (original Android version, 2022-11-11)
# Revision: v2.0 - native Linux build, no Android build-system dependency
#
# Usage (on the board, as a normal user unless stated):
#   make -f npu.mk                 build aipu.ko into $(NPU_OUT)
#   make -f npu.mk check           pre-flight checks only
#   sudo make -f npu.mk install    install module + depmod
#   sudo make -f npu.mk load       modprobe aipu
#   sudo make -f npu.mk unload     rmmod aipu
#   sudo make -f npu.mk headers_install   install the UAPI header for the UMD
#   sudo make -f npu.mk dkms       register/build/install through DKMS
#   make -f npu.mk clean
#
# Cross build from x86_64:
#   make -f npu.mk KDIR=/path/to/linux-7.0 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

NPU_MK_DIR   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# ---------------------------------------------------------------- paths
KVER         ?= $(shell uname -r)
KDIR         ?= /lib/modules/$(KVER)/build
NPU_SRC_PATH ?= $(NPU_MK_DIR)/driver
NPU_OUT      ?= $(NPU_MK_DIR)/out
NPU_UAPI_HDR := $(NPU_SRC_PATH)/armchina-npu/include/armchina_aipu.h

INSTALL_MOD_DIR ?= updates
DESTDIR         ?=
UAPI_INC_DIR    ?= $(DESTDIR)/usr/include/misc
MODULE_DEST     := $(DESTDIR)/lib/modules/$(KVER)/$(INSTALL_MOD_DIR)

# ---------------------------------------------------------------- toolchain
# Native build by default; ARCH/CROSS_COMPILE only for cross builds.
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
ARCH          ?= arm64
CROSS_COMPILE ?=
else
ARCH          ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
endif

# Use LLVM only if the target kernel itself was built with clang.
KCONFIG_AUTO := $(KDIR)/include/config/auto.conf
ifneq ($(wildcard $(KCONFIG_AUTO)),)
ifneq ($(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KCONFIG_AUTO)),)
LLVM ?= 1
endif
endif

# ---------------------------------------------------------------- NPU configs
COMPASS_DRV_BTENVAR_KMD_VERSION ?= 5.11.0
BUILD_AIPU_VERSION_KMD          ?= BUILD_ZHOUYI_V3
BUILD_TARGET_PLATFORM_KMD       ?= BUILD_PLATFORM_SKY1
# DVFS needs scmi_device_*() from the CIX vendor kernel: off on Ubuntu.
BUILD_NPU_DEVFREQ               ?= n
# Set to y if insmod oopses in reserve_iova() on your kernel.
AIPU_SKIP_IOVA_RESERVE          ?= n

NPU_MAKE_ARGS := \
	KDIR=$(KDIR) \
	ARCH=$(ARCH) \
	CROSS_COMPILE=$(CROSS_COMPILE) \
	$(if $(LLVM),LLVM=$(LLVM)) \
	COMPASS_DRV_BTENVAR_KMD_VERSION=$(COMPASS_DRV_BTENVAR_KMD_VERSION) \
	BUILD_AIPU_VERSION_KMD=$(BUILD_AIPU_VERSION_KMD) \
	BUILD_TARGET_PLATFORM_KMD=$(BUILD_TARGET_PLATFORM_KMD) \
	BUILD_NPU_DEVFREQ=$(BUILD_NPU_DEVFREQ) \
	AIPU_SKIP_IOVA_RESERVE=$(AIPU_SKIP_IOVA_RESERVE)

DKMS_NAME    := aipu
DKMS_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(NPU_SRC_PATH)/dkms.conf)

.DEFAULT_GOAL := npu
.PHONY: npu all check install uninstall load unload headers_install \
	headers_uninstall dkms dkms-remove clean

all: npu

# ---------------------------------------------------------------- checks
check:
	@test -f "$(NPU_SRC_PATH)/Makefile" || \
		{ echo "npu: driver sources not found in '$(NPU_SRC_PATH)'"; exit 1; }
	@test -f "$(KDIR)/Makefile" || \
		{ echo "npu: no kernel build tree in '$(KDIR)'"; \
		  echo "     sudo apt install linux-headers-$(KVER)"; exit 1; }
	@test -f "$(KCONFIG_AUTO)" || \
		{ echo "npu: '$(KDIR)' is not configured (no include/config/auto.conf)"; exit 1; }
	@grep -qs '^CONFIG_ARCH_CIX=y' "$(KCONFIG_AUTO)" || \
		echo "npu: WARNING: CONFIG_ARCH_CIX is not set in this kernel"
	@for c in CONFIG_IOMMU_DMA CONFIG_DMA_SHARED_BUFFER CONFIG_PM; do \
		grep -qs "^$$c=y" "$(KCONFIG_AUTO)" || \
			echo "npu: WARNING: $$c is not enabled in this kernel"; \
	done
	@command -v $(if $(LLVM),clang,$(CROSS_COMPILE)gcc) >/dev/null 2>&1 || \
		{ echo "npu: compiler '$(if $(LLVM),clang,$(CROSS_COMPILE)gcc)' not found (apt install build-essential)"; exit 1; }
	@if [ "$(BUILD_NPU_DEVFREQ)" = "y" ] && [ -f "$(KDIR)/Module.symvers" ] && \
	    ! grep -qs 'scmi_device_set_freq' "$(KDIR)/Module.symvers"; then \
		echo "npu: BUILD_NPU_DEVFREQ=y needs the CIX vendor kernel (scmi_device_set_freq missing)"; \
		exit 1; \
	fi
	@echo "npu: kernel $(KVER) - KDIR=$(KDIR) ARCH=$(ARCH) $(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) $(if $(LLVM),LLVM=$(LLVM))"

# ---------------------------------------------------------------- build
npu: check
	$(MAKE) -C $(NPU_SRC_PATH) $(NPU_MAKE_ARGS) modules
	@mkdir -p $(NPU_OUT)
	cp -f $(NPU_SRC_PATH)/aipu.ko $(NPU_OUT)/
	@nm $(NPU_OUT)/aipu.ko 2>/dev/null | grep -q ' init_module$$' || \
		{ echo "npu: aipu.ko has no init_module, build is broken"; exit 1; }
	@echo "npu: built $(NPU_OUT)/aipu.ko"

clean:
	-$(MAKE) -C $(NPU_SRC_PATH) $(NPU_MAKE_ARGS) clean
	rm -rf $(NPU_OUT)

# ---------------------------------------------------------------- install
install: $(NPU_OUT)/aipu.ko
	install -d $(MODULE_DEST)
	install -m 0644 $(NPU_OUT)/aipu.ko $(MODULE_DEST)/aipu.ko
	@if [ -z "$(DESTDIR)" ]; then depmod -a $(KVER); fi
	@echo "npu: installed to $(MODULE_DEST)/aipu.ko"
	@if [ -d /sys/firmware/efi ] && command -v mokutil >/dev/null 2>&1 && \
	    mokutil --sb-state 2>/dev/null | grep -q enabled; then \
		echo "npu: WARNING: Secure Boot is on - aipu.ko must be signed (use 'make -f npu.mk dkms')"; \
	fi

$(NPU_OUT)/aipu.ko:
	@echo "npu: $@ missing, run 'make -f npu.mk' first (as a normal user)"; exit 1

uninstall:
	rm -f $(MODULE_DEST)/aipu.ko
	@if [ -z "$(DESTDIR)" ]; then depmod -a $(KVER); fi

load:
	modprobe aipu
	@ls -l /dev/aipu 2>/dev/null || echo "npu: /dev/aipu not created, see 'dmesg | grep -i aipu'"

unload:
	-rmmod aipu

# UAPI header for the user-mode driver (NOE UMD). Never copied into the
# kernel tree: the module build finds it through -I in driver/Makefile.
headers_install:
	install -d $(UAPI_INC_DIR)
	install -m 0644 $(NPU_UAPI_HDR) $(UAPI_INC_DIR)/armchina_aipu.h

headers_uninstall:
	rm -f $(UAPI_INC_DIR)/armchina_aipu.h

# ---------------------------------------------------------------- DKMS
# Rebuilds automatically on every kernel update and signs the module
# with the DKMS MOK key when Secure Boot is enabled.
dkms:
	@command -v dkms >/dev/null 2>&1 || { echo "npu: apt install dkms"; exit 1; }
	-dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all 2>/dev/null
	rm -rf /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
	cp -a $(NPU_SRC_PATH) /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
	-$(MAKE) -C /usr/src/$(DKMS_NAME)-$(DKMS_VERSION) KDIR=$(KDIR) clean >/dev/null 2>&1
	dkms add -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION) -k $(KVER)

dkms-remove:
	-dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all
	rm -rf /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
