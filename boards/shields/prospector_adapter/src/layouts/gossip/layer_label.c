#include "layer_label.h"

#include <ctype.h>
#include <stdio.h>
#include <zmk/display.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <fonts.h>

/* The active layer's name, grey in a corner; always in capitals */

static lv_obj_t *layer_label;

struct gossip_layer_state {
    uint8_t index;
};

static void gossip_layer_update_cb(struct gossip_layer_state state) {
    if (layer_label == NULL) {
        return;
    }

    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(state.index));
    char text[32];
    if (name && *name) {
        snprintf(text, sizeof(text), "%s", name);
    } else {
        snprintf(text, sizeof(text), "%d", state.index);
    }

    /* The 28 px font holds no lowercase */
    for (int i = 0; text[i]; i++) {
        text[i] = toupper((unsigned char)text[i]);
    }

    lv_label_set_text(layer_label, text);
}

static struct gossip_layer_state gossip_layer_get_state(const zmk_event_t *eh) {
    return (struct gossip_layer_state){.index = zmk_keymap_highest_layer_active()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_gossip_layer, struct gossip_layer_state, gossip_layer_update_cb,
                            gossip_layer_get_state)
ZMK_SUBSCRIPTION(widget_gossip_layer, zmk_layer_state_changed);

lv_obj_t *zmk_widget_gossip_layer_init(lv_obj_t *parent) {
    layer_label = lv_label_create(parent);
    lv_obj_set_style_text_font(layer_label, &DINishCondensed_SemiBold_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(layer_label, lv_color_hex(0x8a8a8a), LV_PART_MAIN);
    lv_label_set_text(layer_label, "");

    widget_gossip_layer_init();
    return layer_label;
}
