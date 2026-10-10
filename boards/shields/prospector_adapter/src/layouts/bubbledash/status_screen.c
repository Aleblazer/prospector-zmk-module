#include <lvgl.h>

#include "bubbledash.h"

lv_obj_t *zmk_display_status_screen() {
    /*
     * A plain black screen that never changes, so LVGL draws it once and
     * then leaves the panel alone; BubbleDash writes every frame itself.
     */
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    bd_start();

    return screen;
}
