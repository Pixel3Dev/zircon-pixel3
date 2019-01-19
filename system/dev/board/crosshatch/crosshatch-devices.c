// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/usb/modeswitch.h>

#include <stdio.h>

#include "crosshatch.h"

#define MMIO_USB3OTG_BASE 0xa600000
#define MMIO_USB3OTG_LENGTH 0xcd00
#define IRQ_USB3 (32 + 133) // GIC_SPI + 133
#define BTI_USB_DWC3 1

static const pbus_mmio_t dwc3_mmios[] = {
    {
        .base = MMIO_USB3OTG_BASE,
        .length = MMIO_USB3OTG_LENGTH,
    },
};

static const pbus_irq_t dwc3_irqs[] = {
    {
        .irq = IRQ_USB3,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const pbus_bti_t dwc3_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_USB_DWC3,
    },
};

static usb_mode_t dwc3_mode = USB_MODE_PERIPHERAL;

static const pbus_metadata_t dwc3_metadata[] = {
    {
        .type        = DEVICE_METADATA_USB_MODE,
        .data_buffer = &dwc3_mode,
        .data_size   = sizeof(dwc3_mode),
    }
};

static const pbus_dev_t dwc3_usb = {
    .name = "dwc3",
    .vid = PDEV_VID_GENERIC,
    .pid = PDEV_PID_GENERIC,
    .did = PDEV_DID_USB_DWC3,
    .mmio_list = dwc3_mmios,
    .mmio_count = countof(dwc3_mmios),
    .irq_list = dwc3_irqs,
    .irq_count = countof(dwc3_irqs),
    .bti_list = dwc3_btis,
    .bti_count = countof(dwc3_btis),
    .metadata_list = dwc3_metadata,
    .metadata_count = countof(dwc3_metadata),
};

static const pbus_dev_t crosshatch_display_dev = {
    .name = "crosshatch-display",
    .vid = PDEV_VID_GOOGLE,
    .did = PDEV_DID_CROSSHATCH_DISPLAY,
};

zx_status_t crosshatch_add_devices(crosshatch_t* crosshatch) {
    zx_status_t status;

    if ((status = pbus_device_add(&crosshatch->pbus, &crosshatch_display_dev)) != ZX_OK) {
        zxlogf(ERROR, "crosshatch_add_devices could not add crosshatch_display_dev: %d\n", status);
    }

    if ((status = pbus_device_add(&crosshatch->pbus, &dwc3_usb)) != ZX_OK) {
        zxlogf(ERROR, "crosshatch_add_devices could not add dwc3_usb: %d\n", status);
    }

    return ZX_OK;
}
