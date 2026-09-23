#include "battery_circles.h"

#include <zmk/display.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_central_status_changed.h>
#include <zmk/event_manager.h>

#include <fonts.h>
#include "display_colors.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

#ifndef PERIPHERAL_COUNT
#define PERIPHERAL_COUNT ZMK_SPLIT_BLE_PERIPHERAL_COUNT
#endif

#define MAX_DISPLAYED (PERIPHERAL_COUNT > 3 ? 3 : PERIPHERAL_COUNT)

/*
 * 124x62 tile. Each peripheral gets a ring with its charge printed inside:
 * 44 px rings for one or two peripherals, 36 px for three, where the
 * condensed 20 px digits still clear the ring's inside.
 */
#define TILE_WIDTH 124
#define TILE_HEIGHT 62
#define ARC_SIZE (MAX_DISPLAYED > 2 ? 36 : 44)
#define ARC_WIDTH (MAX_DISPLAYED > 2 ? 4 : 6)
#define ARC_GAP (MAX_DISPLAYED > 2 ? 3 : 12)

static lv_obj_t *peripheral_arcs[PERIPHERAL_COUNT];
static lv_obj_t *peripheral_labels[PERIPHERAL_COUNT];

/*
 * Every peripheral's charge and link. ZMK's display listener keeps one state
 * value and applies the latest when its work runs, so a state holding only
 * the event's own peripheral lost the other one's update whenever both
 * reported before the display thread got to them; a lost connection left
 * that ring empty for good. Carrying the whole table makes every state
 * complete. It is only touched in the state function, which ZMK serialises.
 */
struct battery_circles_state {
    uint8_t level[PERIPHERAL_COUNT];
    bool connected[PERIPHERAL_COUNT];
};

static struct battery_circles_state battery_table;

static void update_peripheral_display(int i, uint8_t level, bool connected) {
    lv_obj_t *arc = peripheral_arcs[i];
    if (!arc) {
        return;
    }

    lv_arc_set_value(arc, connected ? level : 0);
    lv_obj_set_style_arc_color(arc,
        lv_color_hex(connected ? DISPLAY_COLOR_ARC_INDICATOR : DISPLAY_COLOR_ARC_BG),
        LV_PART_INDICATOR);

    /* Blank until a level arrives, rather than a misleading 0 */
    if (connected && level > 0) {
        lv_label_set_text_fmt(peripheral_labels[i], "%d", level);
    } else {
        lv_label_set_text(peripheral_labels[i], "");
    }
}

static void battery_circles_update_cb(struct battery_circles_state state) {
    struct zmk_widget_battery_circles *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->initialized) {
            for (int i = 0; i < MAX_DISPLAYED; i++) {
                update_peripheral_display(i, state.level[i], state.connected[i]);
            }
        }
    }
}

static struct battery_circles_state battery_circles_get_state(const zmk_event_t *eh) {
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

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_circles, struct battery_circles_state,
                            battery_circles_update_cb, battery_circles_get_state);
ZMK_SUBSCRIPTION(widget_battery_circles, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(widget_battery_circles, zmk_split_central_status_changed);

static lv_obj_t *create_arc(lv_obj_t *parent, int size, int x, int y, int width) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, x, y);

    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);

    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(DISPLAY_COLOR_ARC_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(DISPLAY_COLOR_ARC_BG), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    return arc;
}

int zmk_widget_battery_circles_init(struct zmk_widget_battery_circles *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, TILE_WIDTH, TILE_HEIGHT);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(DISPLAY_COLOR_BATTERY_PANEL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->obj, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->obj, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    const int row_width = MAX_DISPLAYED * ARC_SIZE + (MAX_DISPLAYED - 1) * ARC_GAP;
    const int left_pad = (TILE_WIDTH - row_width) / 2;
    const int top = (TILE_HEIGHT - ARC_SIZE) / 2;

    for (int i = 0; i < MAX_DISPLAYED; i++) {
        peripheral_arcs[i] =
            create_arc(widget->obj, ARC_SIZE, left_pad + i * (ARC_SIZE + ARC_GAP), top, ARC_WIDTH);

        peripheral_labels[i] = lv_label_create(peripheral_arcs[i]);
        lv_label_set_text(peripheral_labels[i], "");
        lv_obj_set_style_text_font(peripheral_labels[i], &DINishCondensed_SemiBold_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(peripheral_labels[i], lv_color_hex(DISPLAY_COLOR_ARC_INDICATOR),
                                    LV_PART_MAIN);
        lv_obj_align(peripheral_labels[i], LV_ALIGN_CENTER, 0, 1);
    }

    widget->initialized = true;
    sys_slist_append(&widgets, &widget->node);
    widget_battery_circles_init();

    return 0;
}

lv_obj_t *zmk_widget_battery_circles_obj(struct zmk_widget_battery_circles *widget) {
    return widget->obj;
}
