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

#define NV3007_PIXEL_SIZE 2U /* RGB565, the only format this driver emits */

/*
 * Longest frame memory axis (NV3007: 168 columns by 428 rows). The clear
 * buffer is sized to this rather than to the narrow axis, because when MV is
 * set the two are exchanged and a row of the clear becomes 428 pixels wide.
 */
#define NV3007_MAX_GRAM_DIM 428U

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
	const uint8_t *extra_init;
	size_t extra_init_len;
};

struct nv3007_data {
	enum display_orientation orientation;
	uint8_t madctl;
	/* Offsets added to the CASET / RASET window for the current MADCTL */
	uint16_t caset_offset;
	uint16_t raset_offset;
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
	NV3007_CMD_COLMOD, 1, NV3007_COLMOD_16BPP,
};

static int nv3007_transmit(const struct device *dev, uint8_t cmd, const uint8_t *tx_data,
			   size_t tx_count)
{
	const struct nv3007_config *config = dev->config;

	return mipi_dbi_command_write(config->mipi_dbi, &config->dbi_config, cmd, tx_data,
				      tx_count);
}

static int nv3007_send_cmd_table(const struct device *dev, const uint8_t *table, size_t len,
				 unsigned int *count)
{
	const uint8_t *const end = table + len;
	const uint8_t *p = table;
	int ret;

	while (p < end) {
		uint8_t cmd = p[0];
		uint8_t n = p[1];

		if (p + 2 + n > end) {
			LOG_ERR("Malformed init table at offset %u", (unsigned int)(p - table));
			return -EINVAL;
		}

		ret = nv3007_transmit(dev, cmd, n ? &p[2] : NULL, n);
		if (ret < 0) {
			LOG_ERR("Init command 0x%02x failed (%d)", cmd, ret);
			return ret;
		}
		p += 2 + n;
		(*count)++;
	}

	return 0;
}

static int nv3007_send_init_seq(const struct device *dev)
{
	const struct nv3007_config *config = dev->config;
	unsigned int count = 0;
	unsigned int extra = 0;
	int ret;

	ret = nv3007_send_cmd_table(dev, nv3007_init_seq, ARRAY_SIZE(nv3007_init_seq), &count);
	if (ret < 0) {
		return ret;
	}

	if (config->extra_init_len) {
		ret = nv3007_send_cmd_table(dev, config->extra_init, config->extra_init_len,
					    &extra);
		if (ret < 0) {
			return ret;
		}
	}

	LOG_INF("Sent %u init commands plus %u overrides", count, extra);
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

static int nv3007_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct nv3007_config *config = dev->config;
	const uint8_t *write_data_start = (const uint8_t *)buf;
	struct display_buffer_descriptor mipi_desc;
	uint16_t nbr_of_writes;
	uint16_t write_h;
	int ret;

	__ASSERT(desc->width <= desc->pitch, "Pitch is smaller than width");
	__ASSERT((desc->pitch * NV3007_PIXEL_SIZE * desc->height) <= desc->buf_size,
		 "Input buffer too small");

	LOG_DBG("Writing %dx%d (w,h) @ %dx%d (x,y) pitch %d", desc->width, desc->height, x, y,
		desc->pitch);

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
 * portrait, and 60h / A0h for the two landscape orientations.
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

	switch (orientation) {
	case DISPLAY_ORIENTATION_NORMAL:
		break;
	case DISPLAY_ORIENTATION_ROTATED_90:
		madctl |= NV3007_MADCTL_MY | NV3007_MADCTL_MV;
		break;
	case DISPLAY_ORIENTATION_ROTATED_180:
		madctl |= NV3007_MADCTL_MY | NV3007_MADCTL_MX;
		break;
	case DISPLAY_ORIENTATION_ROTATED_270:
		madctl |= NV3007_MADCTL_MX | NV3007_MADCTL_MV;
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

	LOG_INF("Orientation %d: MADCTL 0x%02x, offsets col %u row %u", orientation, madctl,
		data->caset_offset, data->raset_offset);

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

#ifdef CONFIG_NV3007_CLEAR_ON_INIT
/*
 * Blank the entire frame memory, not just the visible window.
 *
 * The glass covers 142 of the controller's 168 columns, so 26 columns sit
 * outside anything the application draws. They come up holding noise and the
 * panel shows it as a coloured fringe along one long edge. Black is all
 * zeroes in both 16 and 18 bit formats, so one zeroed buffer serves either.
 */
static uint8_t nv3007_clear_buf[NV3007_MAX_GRAM_DIM * NV3007_PIXEL_SIZE];

static int nv3007_clear_gram(const struct device *dev)
{
	const struct nv3007_config *config = dev->config;
	const struct nv3007_data *data = dev->data;
	struct display_buffer_descriptor desc;
	uint16_t cols = config->gram_width;
	uint16_t rows = config->gram_height;
	uint16_t d[2];
	int ret;

	/*
	 * This runs after the orientation is applied, and MV exchanges the two
	 * address counters, so the window has to be exchanged with it. Getting
	 * this wrong addresses rows as columns and runs off the end of the
	 * frame memory, which only shows up when rotation is set in devicetree
	 * rather than at runtime.
	 */
	if (data->madctl & NV3007_MADCTL_MV) {
		cols = config->gram_height;
		rows = config->gram_width;
	}

	/* One row of the clear is cols pixels wide, so that is what must fit */
	if (cols * NV3007_PIXEL_SIZE > sizeof(nv3007_clear_buf)) {
		return -EINVAL;
	}

	d[0] = sys_cpu_to_be16(0);
	d[1] = sys_cpu_to_be16(cols - 1U);
	ret = nv3007_transmit(dev, NV3007_CMD_CASET, (uint8_t *)d, sizeof(d));
	if (ret < 0) {
		return ret;
	}

	d[0] = sys_cpu_to_be16(0);
	d[1] = sys_cpu_to_be16(rows - 1U);
	ret = nv3007_transmit(dev, NV3007_CMD_RASET, (uint8_t *)d, sizeof(d));
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
	desc.buf_size = cols * NV3007_PIXEL_SIZE;

	for (uint16_t r = 0; r < rows; r++) {
		ret = mipi_dbi_write_display(config->mipi_dbi, &config->dbi_config,
					     nv3007_clear_buf, &desc, PIXEL_FORMAT_RGB_565);
		if (ret < 0) {
			return ret;
		}
	}

	LOG_INF("Cleared %ux%u frame memory", cols, rows);
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
		uint8_t colmod = NV3007_COLMOD_16BPP;

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

	/* Leave the panel blanked; the application turns it on with display_blanking_off() */
	ret = nv3007_blanking_on(dev);
	if (ret < 0) {
		LOG_ERR("Failed to blank display (%d)", ret);
		return ret;
	}

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

/*
 * The {0} default keeps the array valid C when the property is absent. Its
 * length is then reported as 0, so the dummy byte is never read.
 */
#define NV3007_EXTRA_INIT_LEN(inst)                                                              \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, extra_init_cmds),                                \
		    (DT_INST_PROP_LEN(inst, extra_init_cmds)), (0))

#define NV3007_INIT(inst)                                                                        \
	static const uint8_t nv3007_extra_init_##inst[] =                                        \
		DT_INST_PROP_OR(inst, extra_init_cmds, {0});                                     \
                                                                                                 \
	static const struct nv3007_config nv3007_config_##inst = {                               \
		.mipi_dbi = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                 \
		.dbi_config = MIPI_DBI_CONFIG_DT_INST(                                           \
			inst, NV3007_WORD_SIZE(inst) | SPI_OP_MODE_MASTER, 0),                   \
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
		.extra_init = nv3007_extra_init_##inst,                                          \
		.extra_init_len = NV3007_EXTRA_INIT_LEN(inst),                                   \
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
