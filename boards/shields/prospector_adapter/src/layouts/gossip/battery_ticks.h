#pragma once

#include <lvgl.h>

/* Creates a row of small battery bars, one per peripheral, and returns it for placement */
lv_obj_t *zmk_widget_gossip_battery_init(lv_obj_t *parent);
