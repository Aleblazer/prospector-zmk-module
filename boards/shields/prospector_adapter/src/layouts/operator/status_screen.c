#include <lvgl.h>

#include "modifier_indicator.h"
#include "wpm_meter.h"
#include "layer_display.h"
#include "battery_circles.h"
#include "output.h"

/*
 * Operator layout on the 2.79in NV3007 panel, 428x142 landscape canvas.
 *
 *  x:   8        86     136     190                         420
 *      +--------+      +------+ +---------------------------+
 *   6  | batt   |      |      | | WPM meter (230x110)       |
 *      | pills  |      | mods | |                           |
 *  74  +--------+      | 2x2  | |                           |
 *  76  | output |      |      | |                           |
 *      | USB/BLE|      +------+ +---------------------------+
 * 138  +--------+              +--- layer dots (230x6) ----+
 */

static struct zmk_widget_modifier_indicator modifier_indicator_widget;
static struct zmk_widget_wpm_meter wpm_meter_widget;
static struct zmk_widget_layer_display layer_display_widget;
static struct zmk_widget_battery_circles battery_circles_widget;
static struct zmk_widget_output output_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    zmk_widget_battery_circles_init(&battery_circles_widget, screen);
    lv_obj_set_pos(zmk_widget_battery_circles_obj(&battery_circles_widget), 8, 6);

    zmk_widget_output_init(&output_widget, screen);
    lv_obj_set_pos(zmk_widget_output_obj(&output_widget), 8, 76);

    zmk_widget_modifier_indicator_init(&modifier_indicator_widget, screen);
    lv_obj_set_pos(zmk_widget_modifier_indicator_obj(&modifier_indicator_widget), 136, 37);

    zmk_widget_wpm_meter_init(&wpm_meter_widget, screen);
    lv_obj_set_pos(zmk_widget_wpm_meter_obj(&wpm_meter_widget), 190, 6);

    zmk_widget_layer_display_init(&layer_display_widget, screen);
    lv_obj_set_pos(zmk_widget_layer_display_obj(&layer_display_widget), 190, 124);

    return screen;
}
