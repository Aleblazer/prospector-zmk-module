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
static uint8_t levels[PERIPHERAL_COUNT];
static bool connected[PERIPHERAL_COUNT];

static void tick_refresh(uint8_t source) {
    if (source >= TICKS_SHOWN || fills[source] == NULL) {
        return;
    }
    const uint8_t level = connected[source] ? levels[source] : 0;
    lv_obj_set_width(fills[source], (TICK_WIDTH * level + 50) / 100);
    lv_obj_set_style_bg_color(fills[source],
                              lv_color_hex(level < TICK_LOW_LEVEL ? COLOR_LOW : COLOR_FILL),
                              LV_PART_MAIN);
}

struct gossip_battery_state {
    uint8_t source;
    uint8_t level;
};

static void gossip_battery_update_cb(struct gossip_battery_state state) {
    if (state.source >= TICKS_SHOWN) {
        return;
    }
    levels[state.source] = state.level;
    tick_refresh(state.source);
}

static struct gossip_battery_state gossip_battery_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        eh ? as_zmk_peripheral_battery_state_changed(eh) : NULL;
    if (ev == NULL) {
        return (struct gossip_battery_state){.source = 0, .level = levels[0]};
    }
    return (struct gossip_battery_state){.source = ev->source, .level = ev->state_of_charge};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_gossip_battery, struct gossip_battery_state,
                            gossip_battery_update_cb, gossip_battery_get_state)
ZMK_SUBSCRIPTION(widget_gossip_battery, zmk_peripheral_battery_state_changed);

struct gossip_connection_state {
    uint8_t source;
    bool connected;
};

static void gossip_connection_update_cb(struct gossip_connection_state state) {
    if (state.source >= TICKS_SHOWN) {
        return;
    }
    connected[state.source] = state.connected;
    tick_refresh(state.source);
}

static struct gossip_connection_state gossip_connection_get_state(const zmk_event_t *eh) {
    const struct zmk_split_central_status_changed *ev =
        eh ? as_zmk_split_central_status_changed(eh) : NULL;
    if (ev == NULL) {
        return (struct gossip_connection_state){.source = 0, .connected = connected[0]};
    }
    return (struct gossip_connection_state){.source = ev->slot, .connected = ev->connected};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_gossip_connection, struct gossip_connection_state,
                            gossip_connection_update_cb, gossip_connection_get_state)
ZMK_SUBSCRIPTION(widget_gossip_connection, zmk_split_central_status_changed);

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
    widget_gossip_connection_init();

#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
    /* Something to show with nothing paired; real events still replace it */
    static const uint8_t demo_levels[] = {82, 47, 15};
    for (int i = 0; i < TICKS_SHOWN; i++) {
        levels[i] = demo_levels[i];
        connected[i] = true;
        tick_refresh(i);
    }
#endif

    return row;
}
