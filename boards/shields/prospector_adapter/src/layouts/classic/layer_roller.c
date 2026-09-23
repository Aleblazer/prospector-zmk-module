/*
 * Layer carousel for the 428x142 NV3007 canvas.
 *
 * Classic's vertical layer roller, turned sideways to run along the long
 * axis. The active layer sits centred in the regular weight; its neighbours
 * sit either side in the thin weight, fading toward the edges, and the whole
 * strip slides when the layer changes.
 *
 * Spacing is even between the visible edges of the names, not between their
 * centres, so short and long names sit the same distance apart. Each name's
 * width is measured once in both weights, and while a name slides into or out
 * of the centre its width in the spacing sum is blended between the two, so
 * the layout stays continuous even though the rendered weight switches at the
 * midpoint.
 *
 * A fixed set of label slots is relabelled as the strip moves, rather than
 * one label per layer, so it wraps cleanly whatever the layer count, including
 * two layers where the same name must appear on both sides at once.
 *
 * Positions are in thousandths of a layer to keep the maths integer.
 */

#include "layer_roller.h"

#include <stdio.h>
#include <string.h>

#include <zmk/display.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <fonts.h>

#define CAROUSEL_WIDTH 428
#define CAROUSEL_HEIGHT 100
#define CAROUSEL_CENTRE_X (CAROUSEL_WIDTH / 2)

/* Gap between the edges of neighbouring names */
#define CAROUSEL_GAP 32
/* Distance from the centre at which a neighbour has faded out completely */
#define CAROUSEL_FADE_DIST 240
/* Neighbours never exceed this opacity, so the active layer always stands out */
#define CAROUSEL_NEIGHBOUR_OPA 230
#define CAROUSEL_ANIM_MS 250

/* Slots from -CAROUSEL_REACH to +CAROUSEL_REACH around the active layer */
#define CAROUSEL_REACH 3
#define CAROUSEL_SLOTS (2 * CAROUSEL_REACH + 1)

#define CAROUSEL_NAME_MAX 24

#define COLOR_ACTIVE 0xffffff
#define COLOR_NEIGHBOUR 0x909090

#define LAYER_COUNT ZMK_KEYMAP_LAYERS_LEN

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static char names[LAYER_COUNT][CAROUSEL_NAME_MAX];
static int32_t width_active[LAYER_COUNT];
static int32_t width_neighbour[LAYER_COUNT];

static lv_obj_t *slots[CAROUSEL_SLOTS];
/* What each slot currently shows, so labels are only restyled on change */
static int slot_layer[CAROUSEL_SLOTS];
static bool slot_active[CAROUSEL_SLOTS];

/* Current position, in thousandths of a layer; may run outside [0, N) mid-slide */
static int32_t carousel_pos;

static int32_t floor_div(int32_t a, int32_t b) {
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int layer_of(int32_t virtual_index) {
    int32_t m = virtual_index % LAYER_COUNT;
    return m < 0 ? m + LAYER_COUNT : m;
}

/* 1000 when a virtual index is centred, falling to 0 one layer away */
static int32_t centred_weight(int32_t virtual_index, int32_t pos) {
    int32_t d = virtual_index * 1000 - pos;
    if (d < 0) {
        d = -d;
    }
    return d >= 1000 ? 0 : 1000 - d;
}

static int32_t spacing_width(int32_t virtual_index, int32_t pos) {
    int l = layer_of(virtual_index);
    return width_neighbour[l] +
           ((width_active[l] - width_neighbour[l]) * centred_weight(virtual_index, pos)) / 1000;
}

/* Centre-to-centre distance from a virtual index to the next one */
static int32_t step_after(int32_t virtual_index, int32_t pos) {
    return spacing_width(virtual_index, pos) / 2 + CAROUSEL_GAP +
           spacing_width(virtual_index + 1, pos) / 2;
}

static void carousel_layout(int32_t pos) {
    const int32_t base = floor_div(pos, 1000);
    const int32_t frac = pos - base * 1000;
    const int32_t active = floor_div(pos + 500, 1000);
    int32_t offset[CAROUSEL_SLOTS];

    /* Walk outwards from the slot at the base index */
    offset[CAROUSEL_REACH] = -(step_after(base, pos) * frac) / 1000;
    for (int k = 1; k <= CAROUSEL_REACH; k++) {
        offset[CAROUSEL_REACH + k] =
            offset[CAROUSEL_REACH + k - 1] + step_after(base + k - 1, pos);
        offset[CAROUSEL_REACH - k] = offset[CAROUSEL_REACH - k + 1] - step_after(base - k, pos);
    }

    for (int s = 0; s < CAROUSEL_SLOTS; s++) {
        const int32_t v = base + s - CAROUSEL_REACH;
        const int layer = layer_of(v);
        const bool is_active = (v == active);
        lv_obj_t *label = slots[s];

        /* With one layer there are no neighbours to show */
        if (LAYER_COUNT == 1 && !is_active) {
            lv_obj_set_style_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
            continue;
        }

        if (slot_layer[s] != layer) {
            lv_label_set_text_static(label, names[layer]);
            slot_layer[s] = layer;
        }
        if (slot_active[s] != is_active) {
            lv_obj_set_style_text_font(label, is_active ? &FR_Regular_48 : &FR_Thin_48,
                                       LV_PART_MAIN);
            lv_obj_set_style_text_color(
                label, lv_color_hex(is_active ? COLOR_ACTIVE : COLOR_NEIGHBOUR), LV_PART_MAIN);
            slot_active[s] = is_active;
        }

        const int32_t w = is_active ? width_active[layer] : width_neighbour[layer];
        lv_obj_set_x(label, CAROUSEL_CENTRE_X + offset[s] - w / 2);

        int32_t opa = LV_OPA_COVER;
        if (!is_active) {
            int32_t dist = offset[s] < 0 ? -offset[s] : offset[s];
            opa = dist >= CAROUSEL_FADE_DIST
                      ? 0
                      : (CAROUSEL_NEIGHBOUR_OPA * (CAROUSEL_FADE_DIST - dist)) / CAROUSEL_FADE_DIST;
        }
        lv_obj_set_style_opa(label, (lv_opa_t)opa, LV_PART_MAIN);
    }
}

static void carousel_anim_exec(void *var, int32_t value) {
    ARG_UNUSED(var);
    carousel_pos = value;
    carousel_layout(value);
}

static void carousel_anim_completed(lv_anim_t *a) {
    ARG_UNUSED(a);
    /* Fold the position back into [0, N) so it never drifts far from zero */
    const int32_t span = LAYER_COUNT * 1000;
    carousel_pos = ((carousel_pos % span) + span) % span;
    carousel_layout(carousel_pos);
}

/* Slide to a layer the short way round */
static void carousel_go_to(int layer) {
    const int32_t current = floor_div(carousel_pos + 500, 1000);
    int32_t delta = layer - layer_of(current);

    if (delta > LAYER_COUNT / 2) {
        delta -= LAYER_COUNT;
    } else if (delta < -(LAYER_COUNT / 2)) {
        delta += LAYER_COUNT;
    }

    const int32_t target = (current + delta) * 1000;
    if (target == carousel_pos) {
        return;
    }

    lv_anim_delete(slots, carousel_anim_exec);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slots);
    lv_anim_set_exec_cb(&a, carousel_anim_exec);
    lv_anim_set_values(&a, carousel_pos, target);
    lv_anim_set_duration(&a, CAROUSEL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, carousel_anim_completed);
    lv_anim_start(&a);
}

struct layer_roller_state {
    uint8_t index;
};

static bool load_names(void);

static void layer_roller_update_cb(struct layer_roller_state state) {
    /* Pick up layers renamed in ZMK Studio since the last layer change */
    if (load_names()) {
        carousel_layout(carousel_pos);
    }
    carousel_go_to(state.index);
}

static struct layer_roller_state layer_roller_get_state(const zmk_event_t *eh) {
    return (struct layer_roller_state){
        .index = zmk_keymap_highest_layer_active(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_roller, struct layer_roller_state, layer_roller_update_cb,
                            layer_roller_get_state)
ZMK_SUBSCRIPTION(widget_layer_roller, zmk_layer_state_changed);

/* Reads every layer's name and measures it; true if any name changed */
static bool load_names(void) {
    bool changed = false;

    for (int i = 0; i < LAYER_COUNT; i++) {
        const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(i));
        char fresh[CAROUSEL_NAME_MAX];

        if (name && *name) {
            snprintf(fresh, sizeof(fresh), "%s", name);
        } else {
            snprintf(fresh, sizeof(fresh), "%d", i);
        }
        if (strcmp(fresh, names[i]) == 0) {
            continue;
        }
        memcpy(names[i], fresh, sizeof(fresh));
        changed = true;

        /* Slots showing this layer point at the buffer; make them re-read it */
        for (int s = 0; s < CAROUSEL_SLOTS; s++) {
            if (slot_layer[s] == i) {
                slot_layer[s] = -1;
            }
        }

        const uint32_t len = strlen(names[i]);
        width_active[i] = lv_text_get_width(names[i], len, &FR_Regular_48, 0);
        width_neighbour[i] = lv_text_get_width(names[i], len, &FR_Thin_48, 0);
    }
    return changed;
}

int zmk_widget_layer_roller_init(struct zmk_widget_layer_roller *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(widget->obj);
    lv_obj_set_size(widget->obj, CAROUSEL_WIDTH, CAROUSEL_HEIGHT);
    lv_obj_remove_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    load_names();

    for (int s = 0; s < CAROUSEL_SLOTS; s++) {
        slots[s] = lv_label_create(widget->obj);
        lv_obj_set_style_text_font(slots[s], &FR_Thin_48, LV_PART_MAIN);
        lv_obj_set_style_text_color(slots[s], lv_color_hex(COLOR_NEIGHBOUR), LV_PART_MAIN);
        lv_obj_align(slots[s], LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_text_static(slots[s], "");
        slot_layer[s] = -1;
        slot_active[s] = false;
    }

    sys_slist_append(&widgets, &widget->node);

    carousel_pos = (int32_t)zmk_keymap_highest_layer_active() * 1000;
    carousel_layout(carousel_pos);

    widget_layer_roller_init();

    return 0;
}

lv_obj_t *zmk_widget_layer_roller_obj(struct zmk_widget_layer_roller *widget) {
    return widget->obj;
}
