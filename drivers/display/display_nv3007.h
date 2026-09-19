/*
 * Copyright (c) 2026 Aleblazer
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_DISPLAY_DISPLAY_NV3007_H_
#define ZEPHYR_DRIVERS_DISPLAY_DISPLAY_NV3007_H_

#include <zephyr/sys/util.h>

/* Standard MIPI DCS commands understood by the NV3007 */
#define NV3007_CMD_NOP       0x00
#define NV3007_CMD_SWRESET   0x01
#define NV3007_CMD_SLPIN     0x10
#define NV3007_CMD_SLPOUT    0x11
#define NV3007_CMD_NORON     0x13
#define NV3007_CMD_INVOFF    0x20
#define NV3007_CMD_INVON     0x21
#define NV3007_CMD_DISPOFF   0x28
#define NV3007_CMD_DISPON    0x29
#define NV3007_CMD_CASET     0x2A
#define NV3007_CMD_RASET     0x2B
#define NV3007_CMD_RAMWR     0x2C
#define NV3007_CMD_TEON      0x35
#define NV3007_CMD_MADCTL    0x36
#define NV3007_CMD_COLMOD    0x3A

/* Register page select (vendor extension): A5h opens the extended page, 00h closes it */
#define NV3007_CMD_PAGE_SEL  0xFF

/* MADCTL bits (section 6.2.20 of the NV3007 datasheet) */
#define NV3007_MADCTL_MY     BIT(7) /* Page (row) address order: bottom to top */
#define NV3007_MADCTL_MX     BIT(6) /* Column address order: right to left */
#define NV3007_MADCTL_MV     BIT(5) /* Page/column exchange */
#define NV3007_MADCTL_ML     BIT(4) /* Line address order (refresh direction) */
#define NV3007_MADCTL_BGR    BIT(3) /* Colour filter order BGR */
#define NV3007_MADCTL_MH     BIT(2) /* Column scan order (refresh direction) */

/* COLMOD (3Ah) pixel formats */
#define NV3007_COLMOD_16BPP  0x05
#define NV3007_COLMOD_18BPP  0x06

#endif /* ZEPHYR_DRIVERS_DISPLAY_DISPLAY_NV3007_H_ */
