# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := usertest

MODULE_SRCS += \
    $(LOCAL_DIR)/kernel-unittests.cpp \

MODULE_NAME := kernel-unittests

MODULE_STATIC_LIBS := \
    system/ulib/zx \

MODULE_LIBS := \
    system/ulib/c \
    system/ulib/fdio \
    system/ulib/unittest \
    system/ulib/zircon \

MODULE_FIDL_LIBS := \
    system/fidl/fuchsia-device-manager \

include make/module.mk
