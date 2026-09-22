#include <lvgl.h>

#include "key_flight.h"
#include "layer_label.h"
#include "battery_ticks.h"

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    /*
     * 428x142 NV3007 canvas. Typed keys fly across the whole screen; the
     * only fixed elements are the layer name in the bottom-left corner and
     * the battery bars in the bottom-right, both centred on y = 130. They
     * are created after the keys so the keys pass beneath them.
     */
    zmk_widget_key_flight_init(screen);

    lv_obj_t *layer = zmk_widget_gossip_layer_init(screen);
    lv_obj_align(layer, LV_ALIGN_BOTTOM_LEFT, 8, -5);

    lv_obj_t *battery = zmk_widget_gossip_battery_init(screen);
    lv_obj_align(battery, LV_ALIGN_BOTTOM_RIGHT, -8, -11);

    return screen;
}
