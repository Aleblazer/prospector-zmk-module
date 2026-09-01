#include "modifier_indicator.h"

#include <zmk/display.h>
#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
#include <zmk/events/caps_word_state_changed.h>
#endif
#include <zmk/events/keycode_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/hid.h>

#include <fonts.h>
#include <symbols.h>
#include <modifier_order.h>
#include "display_colors.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

#define MOD_GRID_WIDTH 42
#define MOD_CELL_WIDTH 34
#define MOD_CELL_HEIGHT 34
#define MOD_COLUMN_STEP 22
#define MOD_ROW_STEP 30
#define MOD_LEFT_OFFSET -8
#define MOD_TOP_OFFSET 2

struct modifier_indicator_state {
    bool mods[4];
#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
    bool caps_word;
#endif
};

#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
static bool caps_word_active = false;
#endif

static void set_modifier_color(lv_obj_t *label, bool active) {
    lv_color_t color = active ? lv_color_hex(DISPLAY_COLOR_MOD_ACTIVE)
                               : lv_color_hex(DISPLAY_COLOR_MOD_INACTIVE);
    lv_obj_set_style_text_color(label, color, 0);
}

static void modifier_indicator_update_cb(struct modifier_indicator_state state) {
    struct zmk_widget_modifier_indicator *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        for (int i = 0; i < 4; i++) {
            enum modifier_type type = modifier_order_get(i);
#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
            if (type == MOD_TYPE_SHIFT && state.caps_word) {
                lv_obj_set_style_text_color(widget->mod_labels[i],
                    lv_color_hex(DISPLAY_COLOR_MOD_CAPS_WORD), 0);
                continue;
            }
#endif
            set_modifier_color(widget->mod_labels[i], state.mods[type]);
        }
    }
}

static struct modifier_indicator_state modifier_indicator_get_state(const zmk_event_t *eh) {
#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
    if (eh != NULL) {
        const struct zmk_caps_word_state_changed *ev = as_zmk_caps_word_state_changed(eh);
        if (ev != NULL) {
            caps_word_active = ev->active;
        }
    }
#endif

    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();

    struct modifier_indicator_state state = {
        .mods = {false, false, false, false},
#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
        .caps_word = caps_word_active,
#endif
    };

    state.mods[MOD_TYPE_GUI] = (mods & (MOD_LGUI | MOD_RGUI)) != 0;
    state.mods[MOD_TYPE_ALT] = (mods & (MOD_LALT | MOD_RALT)) != 0;
    state.mods[MOD_TYPE_CTRL] = (mods & (MOD_LCTL | MOD_RCTL)) != 0;
    state.mods[MOD_TYPE_SHIFT] = (mods & (MOD_LSFT | MOD_RSFT)) != 0;

    return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_modifier_indicator, struct modifier_indicator_state,
                            modifier_indicator_update_cb, modifier_indicator_get_state)
ZMK_SUBSCRIPTION(widget_modifier_indicator, zmk_keycode_state_changed);

#ifdef CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED
ZMK_SUBSCRIPTION(widget_modifier_indicator, zmk_caps_word_state_changed);
#endif

static lv_obj_t *create_mod_label(lv_obj_t *parent, const char *text, bool use_symbols) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label,
                               use_symbols ? &Symbols_Bold_20
                                           : &DINishCondensed_SemiBold_20,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(DISPLAY_COLOR_MOD_INACTIVE), LV_PART_MAIN);
    lv_obj_center(label);
    return label;
}

int zmk_widget_modifier_indicator_init(struct zmk_widget_modifier_indicator *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, MOD_GRID_WIDTH, 68);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    bool use_symbols = modifier_order_uses_symbols();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *cell = lv_obj_create(widget->obj);
        widget->mod_containers[i] = cell;
        lv_obj_set_size(cell, MOD_CELL_WIDTH, MOD_CELL_HEIGHT);
        lv_obj_set_pos(cell, MOD_LEFT_OFFSET + (i % 2) * MOD_COLUMN_STEP,
                       MOD_TOP_OFFSET + (i / 2) * MOD_ROW_STEP);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        const char *text = use_symbols ? modifier_order_get_symbol(i)
                                       : modifier_order_get_text(i);
        widget->mod_labels[i] = create_mod_label(cell, text, use_symbols);
        if (use_symbols && modifier_order_get(i) == MOD_TYPE_CTRL) {
            lv_obj_align(widget->mod_labels[i], LV_ALIGN_CENTER, 0, 6);
        }
    }

    sys_slist_append(&widgets, &widget->node);
    widget_modifier_indicator_init();

    return 0;
}

lv_obj_t *zmk_widget_modifier_indicator_obj(struct zmk_widget_modifier_indicator *widget) {
    return widget->obj;
}
