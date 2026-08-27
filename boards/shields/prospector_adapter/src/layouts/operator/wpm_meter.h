#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#define WPM_BAR_COUNT 26
#define WPM_MAX 120

#define WPM_METER_WIDTH 158
#define WPM_METER_HEIGHT 68
#define WPM_BAR_WIDTH 5
#define WPM_BAR_GAP 1

struct zmk_widget_wpm_meter {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *bars[WPM_BAR_COUNT];
    lv_obj_t *peak_indicator;
    lv_obj_t *wpm_label;
    lv_obj_t *layer_label;
};

int zmk_widget_wpm_meter_init(struct zmk_widget_wpm_meter *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_wpm_meter_obj(struct zmk_widget_wpm_meter *widget);
