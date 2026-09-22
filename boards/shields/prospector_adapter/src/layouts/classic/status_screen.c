#include <lvgl.h>

#include "layer_roller.h"
#include "battery_bar.h"
#include "modifier_indicator.h"
#include "output.h"

#include <fonts.h>

static struct zmk_widget_layer_roller layer_roller_widget;
static struct zmk_widget_battery_bar battery_bar_widget;
static struct zmk_widget_modifier_indicator modifier_indicator_widget;
static struct zmk_widget_output output_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    /*
     * 428x142 NV3007 canvas. The layer carousel runs along the top 100 px;
     * a 42 px strip underneath carries the battery, the output and the
     * modifiers, centred on y = 121.
     */
    zmk_widget_layer_roller_init(&layer_roller_widget, screen);
    lv_obj_align(zmk_widget_layer_roller_obj(&layer_roller_widget), LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *divider = lv_obj_create(screen);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 404, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 100);

    zmk_widget_battery_bar_init(&battery_bar_widget, screen);
    lv_obj_set_size(zmk_widget_battery_bar_obj(&battery_bar_widget), 184, 40);
    lv_obj_align(zmk_widget_battery_bar_obj(&battery_bar_widget), LV_ALIGN_TOP_LEFT, 12, 101);

    zmk_widget_output_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_obj(&output_widget), LV_ALIGN_TOP_LEFT, 208, 106);

    zmk_widget_modifier_indicator_init(&modifier_indicator_widget, screen);
    lv_obj_align(zmk_widget_modifier_indicator_obj(&modifier_indicator_widget), LV_ALIGN_RIGHT_MID,
                 -8, 50);

    return screen;
}
