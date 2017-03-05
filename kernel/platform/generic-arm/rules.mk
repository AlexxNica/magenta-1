# Copyright 2016 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

WITH_SMP := 1

# 4 silver and 4 gold cores
SMP_MAX_CPUS := 8

LK_HEAP_IMPLEMENTATION ?= cmpctmalloc

MODULE_SRCS += \
	$(LOCAL_DIR)/platform.c \

GLOBAL_DEFINES += \
	ARM_ARCH_WAIT_FOR_SECONDARIES=1

LINKER_SCRIPT += \
	$(BUILDDIR)/system-onesegment.ld

ARCH := arm64
ARM_CPU := cortex-a53

MODULE_DEPS += \
	lib/cbuf \
	lib/mdi \
	dev/pdev \
	dev/timer/arm_generic \
	dev/interrupt/arm_gicv3 \
	dev/psci \
	dev/uart/msm \

include make/module.mk

