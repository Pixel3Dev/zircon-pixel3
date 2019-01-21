// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/io-buffer.h>
#include <ddk/protocol/platform/bus.h>

// BTI IDs for our devices
enum {
    BTI_BOARD,
    BTI_DISPLAY_DUMMY,
};

typedef struct {
    pbus_protocol_t pbus;
    zx_device_t* parent;
    zx_handle_t bti_handle;
} crosshatch_t;

// crosshatch-devices.c
zx_status_t crosshatch_add_devices(crosshatch_t* bus);
