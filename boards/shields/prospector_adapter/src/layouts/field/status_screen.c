#include <lvgl.h>

#include "layer_label.h"
#include "battery_label.h"
#include "line_segments.h"
#include "modifier_indicator.h"
#include "output.h"

#include <fonts.h>

static struct zmk_widget_layer_label layer_label_widget;
static struct zmk_widget_battery_label battery_label_widget;
static struct zmk_widget_line_segments line_segments_widget;
static struct zmk_widget_modifier_indicator modifier_indicator_widget;
static struct zmk_widget_output output_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    /*
     * 428x142 NV3007 canvas. The flow field fills the screen as a 12 x 4
     * grid; the layer name, output and battery float over its left end on
     * rows 0, 1 and 3, each hiding the cells beneath it, and the modifiers
     * take the rightmost column. Label offsets centre each one on its row.
     */
    zmk_widget_line_segments_init(&line_segments_widget, screen);
    lv_obj_set_size(zmk_widget_line_segments_obj(&line_segments_widget),
                    LINE_SEGMENTS_WIDTH, LINE_SEGMENTS_HEIGHT);
    lv_obj_align(zmk_widget_line_segments_obj(&line_segments_widget), LV_ALIGN_CENTER, 0, 0);

    zmk_widget_layer_label_init(&layer_label_widget, screen);
    lv_obj_align(zmk_widget_layer_label_obj(&layer_label_widget), LV_ALIGN_TOP_LEFT, 9, -1);

    zmk_widget_output_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_obj(&output_widget), LV_ALIGN_TOP_LEFT, 4,
                 LINE_SEGMENTS_CELL_Y(LINE_SEGMENTS_OUTPUT_ROW) - LINE_SEGMENTS_SPACING / 2);

    zmk_widget_battery_label_init(&battery_label_widget, screen);
    lv_obj_align(zmk_widget_battery_label_obj(&battery_label_widget), LV_ALIGN_BOTTOM_LEFT, 9, -3);

    zmk_widget_modifier_indicator_init(&modifier_indicator_widget, screen);

    zmk_widget_line_segments_set_labels(&line_segments_widget,
                                        zmk_widget_layer_label_obj(&layer_label_widget),
                                        zmk_widget_battery_label_obj(&battery_label_widget),
                                        zmk_widget_output_obj(&output_widget));

    return screen;
}
