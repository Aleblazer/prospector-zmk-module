/*
 * NewVision NV3007 TFT LCD controller driver (MIPI DBI / 4-wire SPI).
 *
 * The init sequence is the one shipped by the panel vendor (EastRising
 * ER-TFT2.79-1 demo code, NV3007 + 2.79" 142x428 IPS glass) and matches the
 * sequences in LVGL's lv_nv3007 and Arduino_GFX. The driver structure follows
 * Zephyr's ST7789V driver.
 *
 * Copyright (c) 2017 Jan Van Winkel <jan.van_winkel@dxplore.eu>
 * Copyright (c) 2019 Nordic Semiconductor ASA
 * Copyright (c) 2019 Marc Reilly
 * Copyright (c) 2026 Aleblazer
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT newvision_nv3007

#include "display_nv3007.h"

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(display_nv3007, CONFIG_DISPLAY_LOG_LEVEL);

/* The application always hands us RGB565; NV3007_BYTES_PER_PIXEL is what goes
 * out on the wire, which is three bytes in the panel's native 18 bit mode.
 */
#define NV3007_PIXEL_SIZE 2U /* RGB565 in */
#ifdef CONFIG_NV3007_PIXEL_FORMAT_18BIT
#define NV3007_BYTES_PER_PIXEL 3U
#define NV3007_COLMOD_VAL      NV3007_COLMOD_18BPP
#else
#define NV3007_BYTES_PER_PIXEL 2U
#define NV3007_COLMOD_VAL      NV3007_COLMOD_16BPP
#endif

/* Largest frame memory row the driver can transpose in software (NV3007: 168 columns) */
#define NV3007_MAX_GRAM_WIDTH 168U
/* Longest panel axis, for the line buffers below */
#define NV3007_MAX_LINE_PIXELS 428U

#if defined(CONFIG_NV3007_PIXEL_FORMAT_18BIT) || defined(CONFIG_NV3007_INIT_TEST_PATTERN)
#define NV3007_NEEDS_LINE_BUF 1
/* In .bss, never on the stack: driver init runs on the main thread during
 * POST_KERNEL, where CONFIG_MAIN_STACK_SIZE can be as little as 1 KiB.
 */
static uint8_t nv3007_line_buf[NV3007_MAX_LINE_PIXELS * 3U];

/* Expand n RGB565 pixels (big endian on the wire) into RGB666, 3 bytes each */
__maybe_unused static void nv3007_expand_rgb666(uint8_t *dst, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint16_t px = sys_get_be16(&src[i * 2]);

		dst[i * 3 + 0] = (uint8_t)(((px >> 11) & 0x1F) << 3);
		dst[i * 3 + 1] = (uint8_t)(((px >> 5) & 0x3F) << 2);
		dst[i * 3 + 2] = (uint8_t)((px & 0x1F) << 3);
	}
}
#endif

struct nv3007_config {
	const struct device *mipi_dbi;
	const struct mipi_dbi_config dbi_config;
	uint16_t width;
	uint16_t height;
	uint16_t gram_width;
	uint16_t gram_height;
	uint16_t x_offset;
	uint16_t y_offset;
	uint16_t rotation;
	uint16_t ready_time_ms;
	bool bgr;
	bool inversion_on;
};

struct nv3007_data {
	enum display_orientation orientation;
	uint8_t madctl;
	/* Offsets added to the CASET / RASET window for the current MADCTL */
	uint16_t caset_offset;
	uint16_t raset_offset;
	/* Landscape orientation implemented by transposing in software */
	bool transposed;
	uint8_t line[NV3007_MAX_GRAM_WIDTH * NV3007_PIXEL_SIZE];
};

/*
 * Init table format: <cmd> <nparams> <param>... , walked to the end of the
 * array. Everything between PAGE_SEL A5 and PAGE_SEL 00 is vendor register
 * space.
 *
 * There is deliberately no terminator value. The obvious choice, 0xFF, is
 * also NV3007_CMD_PAGE_SEL, which opens and closes that vendor space and so
 * appears twice in the table as a real command. A sentinel loop stopped on
 * the very first entry and silently sent nothing at all.
 */

static const uint8_t nv3007_init_seq[] = {
	NV3007_CMD_PAGE_SEL, 1, 0xA5,
	0x9A, 1, 0x08,
	0x9B, 1, 0x08,
	0x9C, 1, 0xB0,
	0x9D, 1, 0x16,
	0x9E, 1, 0xC4,
	0x8F, 2, 0x55, 0x04,
	0x84, 1, 0x90,
	0x83, 1, 0x7B,
	0x85, 1, 0x33,
	/* Gamma */
	0x60, 1, 0x00,
	0x70, 1, 0x00,
	0x61, 1, 0x02,
	0x71, 1, 0x02,
	0x62, 1, 0x04,
	0x72, 1, 0x04,
	0x6C, 1, 0x29,
	0x7C, 1, 0x29,
	0x6D, 1, 0x31,
	0x7D, 1, 0x31,
	0x6E, 1, 0x0F,
	0x7E, 1, 0x0F,
	0x66, 1, 0x21,
	0x76, 1, 0x21,
	0x68, 1, 0x3A,
	0x78, 1, 0x3A,
	0x63, 1, 0x07,
	0x73, 1, 0x07,
	0x64, 1, 0x05,
	0x74, 1, 0x05,
	0x65, 1, 0x02,
	0x75, 1, 0x02,
	0x67, 1, 0x23,
	0x77, 1, 0x23,
	0x69, 1, 0x08,
	0x79, 1, 0x08,
	0x6A, 1, 0x13,
	0x7A, 1, 0x13,
	0x6B, 1, 0x13,
	0x7B, 1, 0x13,
	0x6F, 1, 0x00,
	0x7F, 1, 0x00,
	/* Source driver */
	0x50, 1, 0x00,
	0x52, 1, 0xD6,
	0x53, 1, 0x08,
	0x54, 1, 0x08,
	0x55, 1, 0x1E,
	0x56, 1, 0x1C,
	/* GOA map_sel */
	0xA0, 3, 0x2B, 0x24, 0x00,
	0xA1, 1, 0x87,
	0xA2, 1, 0x86,
	0xA5, 1, 0x00,
	0xA6, 1, 0x00,
	0xA7, 1, 0x00,
	0xA8, 1, 0x36,
	0xA9, 1, 0x7E,
	0xAA, 1, 0x7E,
	0xB9, 1, 0x85,
	0xBA, 1, 0x84,
	0xBB, 1, 0x83,
	0xBC, 1, 0x82,
	0xBD, 1, 0x81,
	0xBE, 1, 0x80,
	0xBF, 1, 0x01,
	0xC0, 1, 0x02,
	0xC1, 1, 0x00,
	0xC2, 1, 0x00,
	0xC3, 1, 0x00,
	0xC4, 1, 0x33,
	0xC5, 1, 0x7E,
	0xC6, 1, 0x7E,
	0xC8, 2, 0x33, 0x33,
	0xC9, 1, 0x68,
	0xCA, 1, 0x69,
	0xCB, 1, 0x6A,
	0xCC, 1, 0x6B,
	0xCD, 2, 0x33, 0x33,
	0xCE, 1, 0x6C,
	0xCF, 1, 0x6D,
	0xD0, 1, 0x6E,
	0xD1, 1, 0x6F,
	0xAB, 2, 0x03, 0x67,
	0xAC, 2, 0x03, 0x6B,
	0xAD, 2, 0x03, 0x68,
	0xAE, 2, 0x03, 0x6C,
	0xB3, 1, 0x00,
	0xB4, 1, 0x00,
	0xB5, 1, 0x00,
	0xB6, 1, 0x32,
	0xB7, 1, 0x7E,
	0xB8, 1, 0x7E,
	/* Power */
	0xE0, 1, 0x00,
	0xE1, 2, 0x03, 0x0F,
	0xE2, 1, 0x04,
	0xE3, 1, 0x01,
	0xE4, 1, 0x0E,
	0xE5, 1, 0x01,
	0xE6, 1, 0x19,
	0xE7, 1, 0x10,
	0xE8, 1, 0x10,
	0xEA, 1, 0x12,
	0xEB, 1, 0xD0,
	0xEC, 1, 0x04,
	0xED, 1, 0x07,
	0xEE, 1, 0x07,
	0xEF, 1, 0x09,
	0xF0, 1, 0xD0,
	/*
	 * The vendor code writes F1 0E and then a bare data byte 17 with the
	 * "F9" command deliberately commented out, so 17 lands as a second F1
	 * parameter. Reproduce that exactly. (LVGL / Arduino_GFX send F9 17.)
	 */
#ifdef CONFIG_NV3007_INIT_F9
	0xF1, 1, 0x0E,
	0xF9, 1, 0x17,
#else
	0xF1, 2, 0x0E, 0x17,
#endif
	0xF2, 4, 0x2C, 0x1B, 0x0B, 0x20,
	/* 1 dot */
	0xE9, 1, 0x29,
	0xEC, 1, 0x04,
	/* Tearing effect line: on, V-blank only, scanline 0x10 */
	NV3007_CMD_TEON, 1, 0x00,
	0x44, 2, 0x00, 0x10,
	0x46, 1, 0x10,
	NV3007_CMD_PAGE_SEL, 1, 0x00,
	NV3007_CMD_COLMOD, 1, NV3007_COLMOD_VAL,
};

static int nv3007_transmit(const struct device *dev, uint8_t cmd, const uint8_t *tx_data,
			   size_t tx_count)
{
	const struct nv3007_config *config = dev->config;

	return mipi_dbi_command_write(config->mipi_dbi, &config->dbi_config, cmd, tx_data,
				      tx_count);
}

static int nv3007_send_init_seq(const struct device *dev)
{
	const uint8_t *const end = nv3007_init_seq + ARRAY_SIZE(nv3007_init_seq);
	const uint8_t *p = nv3007_init_seq;
	unsigned int count = 0;
	int ret;

	while (p < end) {
		uint8_t cmd = p[0];
		uint8_t n = p[1];

		__ASSERT(p + 2 + n <= end, "malformed NV3007 init table");

		ret = nv3007_transmit(dev, cmd, n ? &p[2] : NULL, n);
		if (ret < 0) {
			LOG_ERR("Init command 0x%02x failed (%d)", cmd, ret);
			return ret;
		}
		p += 2 + n;
		count++;
	}

	LOG_INF("Sent %u init commands (%u bytes of table)", count,
		(unsigned int)ARRAY_SIZE(nv3007_init_seq));
	return 0;
}

static int nv3007_exit_sleep(const struct device *dev)
{
	int ret;

	ret = nv3007_transmit(dev, NV3007_CMD_SLPOUT, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(CONFIG_NV3007_SLEEP_OUT_DELAY_MS));
	return 0;
}

static int nv3007_reset_display(const struct device *dev)
{
	const struct nv3007_config *config = dev->config;
	int ret;

	LOG_DBG("Resetting display");

	ret = mipi_dbi_reset(config->mipi_dbi, CONFIG_NV3007_RESET_HOLD_MS);
	if (ret == -ENOTSUP) {
		/* No reset GPIO on the bus: software reset */
		ret = nv3007_transmit(dev, NV3007_CMD_SWRESET, NULL, 0);
		if (ret < 0) {
			return ret;
		}
	} else if (ret < 0) {
		return ret;
	}

	k_sleep(K_MSEC(CONFIG_NV3007_RESET_RECOVERY_MS));
	return 0;
}

static int nv3007_blanking_on(const struct device *dev)
{
	return nv3007_transmit(dev, NV3007_CMD_DISPOFF, NULL, 0);
}

static int nv3007_blanking_off(const struct device *dev)
{
	return nv3007_transmit(dev, NV3007_CMD_DISPON, NULL, 0);
}

static int nv3007_set_mem_area(const struct device *dev, const uint16_t x, const uint16_t y,
			       const uint16_t w, const uint16_t h)
{
	struct nv3007_data *data = dev->data;
	uint16_t spi_data[2];
	uint16_t ram_x = x + data->caset_offset;
	uint16_t ram_y = y + data->raset_offset;
	int ret;

	spi_data[0] = sys_cpu_to_be16(ram_x);
	spi_data[1] = sys_cpu_to_be16(ram_x + w - 1);
	ret = nv3007_transmit(dev, NV3007_CMD_CASET, (uint8_t *)spi_data, sizeof(spi_data));
	if (ret < 0) {
		return ret;
	}

	spi_data[0] = sys_cpu_to_be16(ram_y);
	spi_data[1] = sys_cpu_to_be16(ram_y + h - 1);
	return nv3007_transmit(dev, NV3007_CMD_RASET, (uint8_t *)spi_data, sizeof(spi_data));
}

BUILD_ASSERT(!IS_ENABLED(CONFIG_NV3007_PIXEL_FORMAT_18BIT) ||
		     IS_ENABLED(CONFIG_NV3007_HW_ROTATION),
	     "18 bit output requires NV3007_HW_ROTATION: the software transpose "
	     "path does not expand pixels");

/*
 * Landscape write without the MV bit: the block (x, y, w, h) in landscape
 * coordinates occupies frame memory columns y..y+h-1 and rows x..x+w-1. The
 * controller fills a window column-first, so send w rows of h pixels, each
 * gathered from one column of the source buffer. MX / MY in MADCTL supply
 * the mirroring that turns a plain transpose into a rotation.
 */
static int nv3007_write_transposed(const struct device *dev, const uint16_t x,
				   const uint16_t y, const struct display_buffer_descriptor *desc,
				   const uint8_t *buf)
{
	const struct nv3007_config *config = dev->config;
	struct nv3007_data *data = dev->data;
	struct display_buffer_descriptor mipi_desc;
	int ret;

	if (desc->height > NV3007_MAX_GRAM_WIDTH) {
		return -EINVAL;
	}

	ret = nv3007_set_mem_area(dev, y, x, desc->height, desc->width);
	if (ret < 0) {
		return ret;
	}

	ret = nv3007_transmit(dev, NV3007_CMD_RAMWR, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	mipi_desc.width = desc->height;
	mipi_desc.pitch = desc->height;
	mipi_desc.height = 1;
	mipi_desc.buf_size = desc->height * NV3007_PIXEL_SIZE;

	for (uint16_t i = 0; i < desc->width; i++) {
		const uint8_t *src = buf + i * NV3007_PIXEL_SIZE;
		uint8_t *dst = data->line;

		for (uint16_t j = 0; j < desc->height; j++) {
			dst[0] = src[0];
			dst[1] = src[1];
			dst += NV3007_PIXEL_SIZE;
			src += desc->pitch * NV3007_PIXEL_SIZE;
		}

		ret = mipi_dbi_write_display(config->mipi_dbi, &config->dbi_config, data->line,
					     &mipi_desc, PIXEL_FORMAT_RGB_565);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

#ifdef CONFIG_NV3007_PIXEL_FORMAT_18BIT
/* Expand each RGB565 line to RGB666 on the way out to the panel */
static int nv3007_write_rgb666(const struct device *dev, const uint16_t x, const uint16_t y,
			       const struct display_buffer_descriptor *desc, const uint8_t *buf)
{
	const struct nv3007_config *config = dev->config;
	struct display_buffer_descriptor mipi_desc;
	int ret;

	if (desc->width > NV3007_MAX_LINE_PIXELS) {
		return -EINVAL;
	}

	ret = nv3007_set_mem_area(dev, x, y, desc->width, desc->height);
	if (ret < 0) {
		return ret;
	}

	ret = nv3007_transmit(dev, NV3007_CMD_RAMWR, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	mipi_desc.width = desc->width;
	mipi_desc.pitch = desc->width;
	mipi_desc.height = 1;
	mipi_desc.buf_size = desc->width * NV3007_BYTES_PER_PIXEL;

	for (uint16_t row = 0; row < desc->height; row++) {
		nv3007_expand_rgb666(nv3007_line_buf,
				     buf + row * desc->pitch * NV3007_PIXEL_SIZE, desc->width);

		ret = mipi_dbi_write_display(config->mipi_dbi, &config->dbi_config,
					     nv3007_line_buf, &mipi_desc, PIXEL_FORMAT_RGB_565);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}
#endif /* CONFIG_NV3007_PIXEL_FORMAT_18BIT */

static int nv3007_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{
	struct nv3007_data *data = dev->data;
	const uint8_t *write_data_start = (const uint8_t *)buf;
#ifndef CONFIG_NV3007_PIXEL_FORMAT_18BIT
	const struct nv3007_config *config = dev->config;
	struct display_buffer_descriptor mipi_desc;
	uint16_t nbr_of_writes;
	uint16_t write_h;
	int ret;
#endif

	__ASSERT(desc->width <= desc->pitch, "Pitch is smaller than width");
	__ASSERT((desc->pitch * NV3007_PIXEL_SIZE * desc->height) <= desc->buf_size,
		 "Input buffer too small");

	LOG_DBG("Writing %dx%d (w,h) @ %dx%d (x,y) pitch %d", desc->width, desc->height, x, y,
		desc->pitch);

	if (data->transposed) {
		return nv3007_write_transposed(dev, x, y, desc, write_data_start);
	}

#ifdef CONFIG_NV3007_PIXEL_FORMAT_18BIT
	return nv3007_write_rgb666(dev, x, y, desc, write_data_start);
#else

	ret = nv3007_set_mem_area(dev, x, y, desc->width, desc->height);
	if (ret < 0) {
		return ret;
	}

	if (desc->pitch > desc->width) {
		write_h = 1U;
		nbr_of_writes = desc->height;
		mipi_desc.height = 1;
		mipi_desc.buf_size = desc->width * NV3007_PIXEL_SIZE;
	} else {
		write_h = desc->height;
		nbr_of_writes = 1U;
		mipi_desc.height = desc->height;
		mipi_desc.buf_size = desc->width * write_h * NV3007_PIXEL_SIZE;
	}

	mipi_desc.width = desc->width;
	/* Per MIPI API, pitch must always match width */
	mipi_desc.pitch = desc->width;

	ret = nv3007_transmit(dev, NV3007_CMD_RAMWR, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	for (uint16_t write_cnt = 0U; write_cnt < nbr_of_writes; ++write_cnt) {
		ret = mipi_dbi_write_display(config->mipi_dbi, &config->dbi_config,
					     write_data_start, &mipi_desc, PIXEL_FORMAT_RGB_565);
		if (ret < 0) {
			return ret;
		}

		write_data_start += (desc->pitch * NV3007_PIXEL_SIZE);
	}

	return 0;
#endif /* CONFIG_NV3007_PIXEL_FORMAT_18BIT */
}

static void nv3007_get_capabilities(const struct device *dev,
				    struct display_capabilities *capabilities)
{
	const struct nv3007_config *config = dev->config;
	const struct nv3007_data *data = dev->data;

	memset(capabilities, 0, sizeof(struct display_capabilities));

	if (data->orientation == DISPLAY_ORIENTATION_ROTATED_90 ||
	    data->orientation == DISPLAY_ORIENTATION_ROTATED_270) {
		capabilities->x_resolution = config->height;
		capabilities->y_resolution = config->width;
	} else {
		capabilities->x_resolution = config->width;
		capabilities->y_resolution = config->height;
	}

	capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	capabilities->current_pixel_format = PIXEL_FORMAT_RGB_565;
	capabilities->current_orientation = data->orientation;
}

static int nv3007_set_pixel_format(const struct device *dev,
				   const enum display_pixel_format pixel_format)
{
	if (pixel_format == PIXEL_FORMAT_RGB_565) {
		return 0;
	}

	LOG_ERR("Pixel format change not implemented");
	return -ENOTSUP;
}

/*
 * Work out MADCTL and the GRAM window offsets for an orientation.
 *
 * The glass is smaller than the controller's frame memory (142 of 168 columns
 * on the 2.79" panels). x-offset/y-offset give the margin before the first
 * visible pixel in the native orientation; when an axis is mirrored (MX / MY)
 * the margin on the other side applies instead. With MV set the column
 * counter (CASET) walks the panel's row axis and vice versa.
 *
 * MADCTL values match the vendor demo: 00h portrait, C0h upside-down
 * portrait, 60h / A0h for the two landscape orientations when
 * CONFIG_NV3007_HW_ROTATION is set. By default landscape keeps the vendor's
 * portrait addressing (MADCTL 40h / 80h) and the driver transposes.
 */
static int nv3007_apply_orientation(const struct device *dev,
				    const enum display_orientation orientation)
{
	const struct nv3007_config *config = dev->config;
	struct nv3007_data *data = dev->data;
	uint8_t madctl = config->bgr ? NV3007_MADCTL_BGR : 0;
	uint16_t col_margin;
	uint16_t row_margin;
	int ret;

	bool hw_mv = IS_ENABLED(CONFIG_NV3007_HW_ROTATION);
	bool transposed = false;

	switch (orientation) {
	case DISPLAY_ORIENTATION_NORMAL:
		break;
	case DISPLAY_ORIENTATION_ROTATED_90:
		madctl |= NV3007_MADCTL_MY;
		if (hw_mv) {
			madctl |= NV3007_MADCTL_MV;
		} else {
			transposed = true;
		}
		break;
	case DISPLAY_ORIENTATION_ROTATED_180:
		madctl |= NV3007_MADCTL_MY | NV3007_MADCTL_MX;
		break;
	case DISPLAY_ORIENTATION_ROTATED_270:
		madctl |= NV3007_MADCTL_MX;
		if (hw_mv) {
			madctl |= NV3007_MADCTL_MV;
		} else {
			transposed = true;
		}
		break;
	default:
		return -ENOTSUP;
	}

	/*
	 * The visible glass is inset in the frame memory, so a mirrored axis
	 * counts its margin from the other side. MV exchanges the two address
	 * counters, which also exchanges which mirror bit governs which axis:
	 * with MV set, CASET walks panel rows (mirrored by MX) and RASET walks
	 * panel columns (mirrored by MY). That is what makes MADCTL A0h need a
	 * 14 pixel RASET offset on a 142-of-168 column panel, matching the
	 * esp_lcd_nv3007 reference driver.
	 */
	if (madctl & NV3007_MADCTL_MV) {
		col_margin = (madctl & NV3007_MADCTL_MY)
				     ? (config->gram_width - config->width - config->x_offset)
				     : config->x_offset;
		row_margin = (madctl & NV3007_MADCTL_MX)
				     ? (config->gram_height - config->height - config->y_offset)
				     : config->y_offset;
		data->caset_offset = row_margin;
		data->raset_offset = col_margin;
	} else {
		col_margin = (madctl & NV3007_MADCTL_MX)
				     ? (config->gram_width - config->width - config->x_offset)
				     : config->x_offset;
		row_margin = (madctl & NV3007_MADCTL_MY)
				     ? (config->gram_height - config->height - config->y_offset)
				     : config->y_offset;
		data->caset_offset = col_margin;
		data->raset_offset = row_margin;
	}

	ret = nv3007_transmit(dev, NV3007_CMD_MADCTL, &madctl, 1U);
	if (ret < 0) {
		return ret;
	}

	data->madctl = madctl;
	data->orientation = orientation;
	data->transposed = transposed;

	LOG_INF("Orientation %d: MADCTL 0x%02x, offsets col %u row %u%s", orientation, madctl,
		data->caset_offset, data->raset_offset, transposed ? ", transposed" : "");

	return 0;
}

static int nv3007_set_orientation(const struct device *dev,
				  const enum display_orientation orientation)
{
	int ret = nv3007_apply_orientation(dev, orientation);

	if (ret == -ENOTSUP) {
		LOG_ERR("Unsupported display orientation %d", orientation);
	}

	return ret;
}

static enum display_orientation nv3007_rotation_to_orientation(uint16_t rotation)
{
	switch (rotation) {
	case 90:
		return DISPLAY_ORIENTATION_ROTATED_90;
	case 180:
		return DISPLAY_ORIENTATION_ROTATED_180;
	case 270:
		return DISPLAY_ORIENTATION_ROTATED_270;
	default:
		return DISPLAY_ORIENTATION_NORMAL;
	}
}

#ifdef CONFIG_NV3007_INIT_TEST_PATTERN
/*
 * Geometry and byte-ratio probe, designed to be read off a single photograph.
 *
 * It paints three white bands across the long axis, each a different width so
 * they can be told apart no matter which end of the panel row zero lands on:
 *
 *   band A,  40 rows wide: the driver's column window, 2 bytes per pixel
 *   band B,  60 rows wide: the driver's column window, 3 bytes per pixel
 *   band C,  80 rows wide: the whole frame memory width, deliberately overfed
 *
 * How far each band reaches along the short axis is the measurement. A and B
 * differ only in bytes per pixel, so whichever one reaches the full height of
 * the glass is the format the controller is really using. C is fed more data
 * than any interpretation needs, so it shows the true visible extent: if C is
 * taller than both A and B, the configured width and offset are wrong.
 */
static int nv3007_raw_window(const struct device *dev, uint16_t c0, uint16_t c1, uint16_t r0,
			     uint16_t r1)
{
	uint16_t d[2];
	int ret;

	d[0] = sys_cpu_to_be16(c0);
	d[1] = sys_cpu_to_be16(c1);
	ret = nv3007_transmit(dev, NV3007_CMD_CASET, (uint8_t *)d, sizeof(d));
	if (ret < 0) {
		return ret;
	}

	d[0] = sys_cpu_to_be16(r0);
	d[1] = sys_cpu_to_be16(r1);
	return nv3007_transmit(dev, NV3007_CMD_RASET, (uint8_t *)d, sizeof(d));
}

static int nv3007_raw_fill(const struct device *dev, uint16_t c0, uint16_t c1, uint16_t r0,
			   uint16_t r1, uint16_t color, size_t bpp)
{
	const struct nv3007_config *config = dev->config;
	struct display_buffer_descriptor desc;
	uint16_t cols = c1 - c0 + 1U;
	int ret;

	if (cols > NV3007_MAX_LINE_PIXELS) {
		return -ENOMEM;
	}

	for (uint16_t i = 0; i < cols; i++) {
		if (bpp == 3U) {
			nv3007_line_buf[i * 3 + 0] = (uint8_t)(((color >> 11) & 0x1F) << 3);
			nv3007_line_buf[i * 3 + 1] = (uint8_t)(((color >> 5) & 0x3F) << 2);
			nv3007_line_buf[i * 3 + 2] = (uint8_t)((color & 0x1F) << 3);
		} else {
			nv3007_line_buf[i * 2 + 0] = (uint8_t)(color >> 8);
			nv3007_line_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
		}
	}

	ret = nv3007_raw_window(dev, c0, c1, r0, r1);
	if (ret < 0) {
		return ret;
	}

	ret = nv3007_transmit(dev, NV3007_CMD_RAMWR, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	desc.width = cols;
	desc.pitch = cols;
	desc.height = 1;
	desc.buf_size = cols * bpp;

	for (uint16_t r = r0; r <= r1; r++) {
		ret = mipi_dbi_write_display(config->mipi_dbi, &config->dbi_config,
					     nv3007_line_buf, &desc, PIXEL_FORMAT_RGB_565);
		if (ret < 0) {
			return ret;
		}
	}

	LOG_INF("fill cols %u-%u rows %u-%u, %u bpp, %u bytes", c0, c1, r0, r1,
		(unsigned int)bpp, (unsigned int)((r1 - r0 + 1U) * cols * bpp));
	return 0;
}

static int nv3007_paint_test_pattern(const struct device *dev)
{
	/*
	 * A ruler across the short axis, in three acts.
	 *
	 * Act 1 paints seven wide bands, each spanning every row so it runs
	 * the whole length of the glass, and each 22 frame memory columns
	 * wide, which is about 3.3 mm and a seventh of the panel's width.
	 * Red marks the low column end and green the high one, with white
	 * bands between them, so a photograph says directly which part of the
	 * column range reaches the glass and at what scale.
	 *
	 * Every window stays inside columns 12 to 153. That is the only range
	 * that has ever lit this panel, and the previous two probes, which
	 * both strayed outside it and then showed nothing at all, suggest
	 * asking for other columns wedges the address logic.
	 *
	 * Act 2 tests that suspicion on purpose by writing one band above the
	 * range. Act 3 repaints the ruler: if act 1 was visible and act 3 is
	 * not, the out of range write is what breaks the controller, and the
	 * driver must clamp to the safe window.
	 */
	static const struct {
		uint16_t c0, c1;
		uint16_t color;
		const char *name;
	} bands[] = {
		{12, 33, 0xF800, "red"},    {34, 55, 0x0000, "black"},
		{56, 77, 0xFFFF, "white"},  {78, 99, 0x0000, "black"},
		{100, 121, 0xFFFF, "white"}, {122, 143, 0x0000, "black"},
		{144, 153, 0x07E0, "green"},
	};
	const struct nv3007_config *config = dev->config;
	struct nv3007_data *data = dev->data;
	uint16_t last_row = config->gram_height - 1U;
	uint8_t colmod = NV3007_COLMOD_16BPP;
	int ret;

	ret = nv3007_blanking_off(dev);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("probe: panel %ux%u, GRAM %ux%u, x-offset %u, MADCTL 0x%02x", config->width,
		config->height, config->gram_width, config->gram_height, config->x_offset,
		data->madctl);

	(void)nv3007_transmit(dev, NV3007_CMD_COLMOD, &colmod, 1);

	for (int pass = 0; pass < 2; pass++) {
		LOG_INF("probe: act %d, ruler across columns 12-153", pass == 0 ? 1 : 3);

		for (size_t i = 0; i < ARRAY_SIZE(bands); i++) {
			ret = nv3007_raw_fill(dev, bands[i].c0, bands[i].c1, 0, last_row,
					      bands[i].color, 3U);
			LOG_INF("probe: band %s cols %u-%u -> %d", bands[i].name, bands[i].c0,
				bands[i].c1, ret);
		}

		k_sleep(K_MSEC(CONFIG_NV3007_INIT_TEST_PATTERN_HOLD_MS));

		if (pass == 0) {
			LOG_INF("probe: act 2, one band above the safe range (cols 154-167)");
			ret = nv3007_raw_fill(dev, 154, config->gram_width - 1U, 0, last_row,
					      0xFFFF, 3U);
			LOG_INF("probe: out of range band -> %d", ret);
			k_sleep(K_MSEC(CONFIG_NV3007_INIT_TEST_PATTERN_HOLD_MS));
		}
	}

	LOG_INF("probe: done");

	return 0;
}
#endif /* CONFIG_NV3007_INIT_TEST_PATTERN */

#ifdef CONFIG_NV3007_CLEAR_ON_INIT
/*
 * Blank the entire frame memory, not just the visible window.
 *
 * The glass covers 142 of the controller's 168 columns, so 26 columns sit
 * outside anything the application draws. They come up holding noise and the
 * panel shows it as a coloured fringe along one long edge. Black is all
 * zeroes in both 16 and 18 bit formats, so one zeroed buffer serves either.
 */
static uint8_t nv3007_clear_buf[NV3007_MAX_GRAM_WIDTH * 3U];

static int nv3007_clear_gram(const struct device *dev)
{
	const struct nv3007_config *config = dev->config;
	struct display_buffer_descriptor desc;
	uint16_t d[2];
	int ret;

	if (config->gram_width > NV3007_MAX_GRAM_WIDTH) {
		return -EINVAL;
	}

	d[0] = sys_cpu_to_be16(0);
	d[1] = sys_cpu_to_be16(config->gram_width - 1U);
	ret = nv3007_transmit(dev, NV3007_CMD_CASET, (uint8_t *)d, sizeof(d));
	if (ret < 0) {
		return ret;
	}

	d[0] = sys_cpu_to_be16(0);
	d[1] = sys_cpu_to_be16(config->gram_height - 1U);
	ret = nv3007_transmit(dev, NV3007_CMD_RASET, (uint8_t *)d, sizeof(d));
	if (ret < 0) {
		return ret;
	}

	ret = nv3007_transmit(dev, NV3007_CMD_RAMWR, NULL, 0);
	if (ret < 0) {
		return ret;
	}

	desc.width = config->gram_width;
	desc.pitch = config->gram_width;
	desc.height = 1;
	desc.buf_size = config->gram_width * NV3007_BYTES_PER_PIXEL;

	for (uint16_t r = 0; r < config->gram_height; r++) {
		ret = mipi_dbi_write_display(config->mipi_dbi, &config->dbi_config,
					     nv3007_clear_buf, &desc, PIXEL_FORMAT_RGB_565);
		if (ret < 0) {
			return ret;
		}
	}

	LOG_INF("Cleared %ux%u frame memory", config->gram_width, config->gram_height);
	return 0;
}
#endif /* CONFIG_NV3007_CLEAR_ON_INIT */

static int nv3007_init(const struct device *dev)
{
	const struct nv3007_config *config = dev->config;
	int ret;

	if (!device_is_ready(config->mipi_dbi)) {
		LOG_ERR("MIPI DBI device not ready");
		return -ENODEV;
	}

	if (config->width + config->x_offset > config->gram_width ||
	    config->height + config->y_offset > config->gram_height) {
		LOG_ERR("Panel %ux%u @ (%u,%u) does not fit %ux%u frame memory", config->width,
			config->height, config->x_offset, config->y_offset, config->gram_width,
			config->gram_height);
		return -EINVAL;
	}

	k_sleep(K_TIMEOUT_ABS_MS(config->ready_time_ms));

	ret = nv3007_reset_display(dev);
	if (ret < 0) {
		LOG_ERR("Failed to reset display (%d)", ret);
		return ret;
	}

	/*
	 * Order matters, and it is the order esp_lcd_nv3007 uses for this
	 * panel: leave sleep first, then set the access and pixel format, then
	 * program the vendor register page. The panel powers up asleep with its
	 * analog blocks off, and writes to the vendor page do not stick until
	 * it is awake, which looks exactly like a dead SPI link.
	 */
	ret = nv3007_exit_sleep(dev);
	if (ret < 0) {
		LOG_ERR("Failed to exit sleep mode (%d)", ret);
		return ret;
	}

	ret = nv3007_apply_orientation(dev, nv3007_rotation_to_orientation(config->rotation));
	if (ret < 0) {
		LOG_ERR("Failed to set orientation (%d)", ret);
		return ret;
	}

	{
		uint8_t colmod = NV3007_COLMOD_VAL;

		ret = nv3007_transmit(dev, NV3007_CMD_COLMOD, &colmod, 1);
		if (ret < 0) {
			return ret;
		}
	}

	ret = nv3007_send_init_seq(dev);
	if (ret < 0) {
		LOG_ERR("Failed to init display (%d)", ret);
		return ret;
	}

	/* The vendor sequence ends with its own Sleep Out; keep that timing */
	ret = nv3007_exit_sleep(dev);
	if (ret < 0) {
		LOG_ERR("Failed to exit sleep mode (%d)", ret);
		return ret;
	}

	ret = nv3007_transmit(dev, config->inversion_on ? NV3007_CMD_INVON : NV3007_CMD_INVOFF,
			      NULL, 0);
	if (ret < 0) {
		return ret;
	}

#ifdef CONFIG_NV3007_CLEAR_ON_INIT
	ret = nv3007_clear_gram(dev);
	if (ret < 0) {
		LOG_ERR("Failed to clear frame memory (%d)", ret);
		return ret;
	}
#endif

#ifdef CONFIG_NV3007_INIT_TEST_PATTERN
	ret = nv3007_paint_test_pattern(dev);
	if (ret < 0) {
		LOG_ERR("Failed to paint test pattern (%d)", ret);
		return ret;
	}
#else
	/* Leave the panel blanked; the application turns it on with display_blanking_off() */
	ret = nv3007_blanking_on(dev);
	if (ret < 0) {
		LOG_ERR("Failed to blank display (%d)", ret);
		return ret;
	}
#endif

	LOG_INF("NV3007 ready: %ux%u panel in %ux%u GRAM, rotation %u", config->width,
		config->height, config->gram_width, config->gram_height, config->rotation);

	return 0;
}

#ifdef CONFIG_PM_DEVICE
static int nv3007_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		return nv3007_exit_sleep(dev);
	case PM_DEVICE_ACTION_SUSPEND:
		return nv3007_transmit(dev, NV3007_CMD_SLPIN, NULL, 0);
	default:
		return -ENOTSUP;
	}
}
#endif /* CONFIG_PM_DEVICE */

static DEVICE_API(display, nv3007_api) = {
	.blanking_on = nv3007_blanking_on,
	.blanking_off = nv3007_blanking_off,
	.write = nv3007_write,
	.get_capabilities = nv3007_get_capabilities,
	.set_pixel_format = nv3007_set_pixel_format,
	.set_orientation = nv3007_set_orientation,
};

#define NV3007_WORD_SIZE(inst)                                                                   \
	((DT_INST_STRING_UPPER_TOKEN(inst, mipi_mode) == MIPI_DBI_MODE_SPI_4WIRE)                \
		 ? SPI_WORD_SET(8)                                                               \
		 : SPI_WORD_SET(9))

#ifdef CONFIG_NV3007_SPI_MODE3
#define NV3007_SPI_MODE_FLAGS (SPI_MODE_CPOL | SPI_MODE_CPHA)
#else
#define NV3007_SPI_MODE_FLAGS 0
#endif

#define NV3007_INIT(inst)                                                                        \
	static const struct nv3007_config nv3007_config_##inst = {                               \
		.mipi_dbi = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                 \
		.dbi_config = MIPI_DBI_CONFIG_DT_INST(                                           \
			inst, NV3007_WORD_SIZE(inst) | SPI_OP_MODE_MASTER |                       			NV3007_SPI_MODE_FLAGS, 0),                                               \
		.width = DT_INST_PROP(inst, width),                                              \
		.height = DT_INST_PROP(inst, height),                                            \
		.gram_width = DT_INST_PROP(inst, gram_width),                                    \
		.gram_height = DT_INST_PROP(inst, gram_height),                                  \
		.x_offset = DT_INST_PROP(inst, x_offset),                                        \
		.y_offset = DT_INST_PROP(inst, y_offset),                                        \
		.rotation = DT_INST_PROP(inst, rotation),                                        \
		.ready_time_ms = DT_INST_PROP(inst, ready_time_ms),                              \
		.bgr = DT_INST_PROP(inst, bgr),                                                  \
		.inversion_on = DT_INST_PROP(inst, inversion_on),                                \
	};                                                                                       \
                                                                                                 \
	static struct nv3007_data nv3007_data_##inst = {                                         \
		.orientation = DISPLAY_ORIENTATION_NORMAL,                                       \
	};                                                                                       \
                                                                                                 \
	PM_DEVICE_DT_INST_DEFINE(inst, nv3007_pm_action);                                        \
                                                                                                 \
	DEVICE_DT_INST_DEFINE(inst, &nv3007_init, PM_DEVICE_DT_INST_GET(inst),                   \
			      &nv3007_data_##inst, &nv3007_config_##inst, POST_KERNEL,           \
			      CONFIG_DISPLAY_INIT_PRIORITY, &nv3007_api);

DT_INST_FOREACH_STATUS_OKAY(NV3007_INIT)
