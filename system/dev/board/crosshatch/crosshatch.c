// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>

#include <zircon/assert.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/threads.h>

#include "crosshatch.h"


static void crosshatch_release(void* ctx) {
    crosshatch_t* hikey = ctx;

    zx_handle_close(hikey->bti_handle);
    free(hikey);
}

static zx_protocol_device_t crosshatch_device_protocol = {
    .version = DEVICE_OPS_VERSION,
    .release = crosshatch_release,
};

static zx_status_t crosshatch_bind(void* ctx, zx_device_t* parent) {
    crosshatch_t* hikey = calloc(1, sizeof(crosshatch_t));
    if (!hikey) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t status = device_get_protocol(parent, ZX_PROTOCOL_PBUS, &hikey->pbus);
    if (status != ZX_OK) {
        free(hikey);
        return ZX_ERR_NOT_SUPPORTED;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "crosshatch",
        .ctx = hikey,
        .ops = &crosshatch_device_protocol,
        // nothing should bind to this device
        // all interaction will be done via the pbus_interface_t
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    status = device_add(parent, &args, NULL);
    if (status != ZX_OK) {
        goto fail;
    }

    return ZX_OK;

fail:
    zxlogf(ERROR, "crosshatch_bind failed %d\n", status);
    crosshatch_release(hikey);
    return status;
}

static zx_driver_ops_t crosshatch_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = crosshatch_bind,
};

ZIRCON_DRIVER_BEGIN(crosshatch, crosshatch_driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PBUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GOOGLE),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_CROSSHATCH),
ZIRCON_DRIVER_END(crosshatch)
