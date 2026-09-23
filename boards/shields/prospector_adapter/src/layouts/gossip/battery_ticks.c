#include "battery_ticks.h"

#include <zmk/display.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_central_status_changed.h>
#include <zmk/event_manager.h>

/*
 * One small bar per peripheral, in pairing order: grey fill over a dim
 * track, amber below 20%. A disconnected peripheral shows an empty track.
 */

#ifndef PERIPHERAL_COUNT
#define PERIPHERAL_COUNT ZMK_SPLIT_BLE_PERIPHERAL_COUNT
#endif

#define TICKS_SHOWN (PERIPHERAL_COUNT > 3 ? 3 : PERIPHERAL_COUNT)

#define TICK_WIDTH 28
#define TICK_HEIGHT 6
#define TICK_GAP 8
#define TICK_RADIUS 2
#define TICK_LOW_LEVEL 20

#define COLOR_TRACK 0x303030
#define COLOR_FILL 0x8a8a8a
#define COLOR_LOW 0xe0a020

static lv_obj_t *fills[PERIPHERAL_COUNT];

/*
 * Every peripheral's charge and link. ZMK's display listener keeps one state
 * value and applies the latest when its work runs, so a state holding only
 * the event's own peripheral lost the other one's update whenever both
 * reported before the display thread got to them; a lost connection left
 * that bar empty for good. Carrying the whole table makes every state
 * complete. It is only touched in the state function, which ZMK serialises.
 */
struct gossip_battery_state {
    uint8_t level[PERIPHERAL_COUNT];
    bool connected[PERIPHERAL_COUNT];
};

static struct gossip_battery_state battery_table;

static void tick_show(int i, uint8_t level) {
    if (fills[i] == NULL) {
        return;
    }
    lv_obj_set_width(fills[i], (TICK_WIDTH * level + 50) / 100);
    lv_obj_set_style_bg_color(fills[i],
                              lv_color_hex(level < TICK_LOW_LEVEL ? COLOR_LOW : COLOR_FILL),
                              LV_PART_MAIN);
}

static void gossip_battery_update_cb(struct gossip_battery_state state) {
    for (int i = 0; i < TICKS_SHOWN; i++) {
        tick_show(i, state.connected[i] ? state.level[i] : 0);
    }
}

static struct gossip_battery_state gossip_battery_get_state(const zmk_event_t *eh) {
    if (eh != NULL) {
        const struct zmk_peripheral_battery_state_changed *bat =
            as_zmk_peripheral_battery_state_changed(eh);
        if (bat != NULL && bat->source < PERIPHERAL_COUNT) {
            battery_table.level[bat->source] = bat->state_of_charge;
        }

        const struct zmk_split_central_status_changed *conn =
            as_zmk_split_central_status_changed(eh);
        if (conn != NULL && conn->slot < PERIPHERAL_COUNT) {
            battery_table.connected[conn->slot] = conn->connected;
        }
    }
    return battery_table;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_gossip_battery, struct gossip_battery_state,
                            gossip_battery_update_cb, gossip_battery_get_state)
ZMK_SUBSCRIPTION(widget_gossip_battery, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(widget_gossip_battery, zmk_split_central_status_changed);

lv_obj_t *zmk_widget_gossip_battery_init(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* START: a content-sized row must grow from the start, or it clips its children */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TICK_GAP, LV_PART_MAIN);

    for (int i = 0; i < TICKS_SHOWN; i++) {
        lv_obj_t *track = lv_obj_create(row);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, TICK_WIDTH, TICK_HEIGHT);
        lv_obj_set_style_bg_color(track, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(track, TICK_RADIUS, LV_PART_MAIN);

        fills[i] = lv_obj_create(track);
        lv_obj_remove_style_all(fills[i]);
        lv_obj_set_size(fills[i], 0, TICK_HEIGHT);
        lv_obj_set_style_bg_opa(fills[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(fills[i], TICK_RADIUS, LV_PART_MAIN);
    }

    widget_gossip_battery_init();

    return row;
}
