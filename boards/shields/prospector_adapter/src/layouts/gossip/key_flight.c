#include "key_flight.h"

#include <math.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>

#include <fonts.h>

/*
 * Every printable key pressed on the keyboard appears at a random spot on
 * the screen and flies out toward the viewer along a random bent path:
 * growing as it nears, drifting off its heading and fading as it passes.
 *
 * Perspective is faked with a stepped set of font sizes rather than LVGL's
 * transform scaling, which is slow on this chip and blurs text. Keys are
 * queued by the event listener and launched from an LVGL timer, so all
 * LVGL calls stay on the display thread.
 */

#define SCREEN_WIDTH 428
#define SCREEN_HEIGHT 142

/* Keys in flight at once; a new key takes over the oldest when all are busy */
#define FLIGHT_POOL 12
#define FRAME_MS 30

/* A key starts 18 px tall and flies until it is 18 / 0.18 = 100 px */
#define START_SIZE 18.0f
#define NEAREST_Z 0.18f

#define PI_F 3.14159265f

static const lv_font_t *const flight_fonts[] = {
    &DINish_SemiBold_18, &DINish_SemiBold_24, &DINish_SemiBold_32, &DINish_SemiBold_42,
    &DINish_SemiBold_56, &DINish_SemiBold_74, &DINish_SemiBold_96,
};
static const uint8_t flight_font_px[] = {18, 24, 32, 42, 56, 74, 96};
#define FLIGHT_FONT_COUNT ARRAY_SIZE(flight_fonts)

struct flight {
    lv_obj_t *label;
    bool active;
    uint8_t font;
    uint32_t start;
    uint32_t duration;
    /* Launch point on screen, heading, how far it travels and how far its path bends */
    float ox, oy;
    float ux, uy;
    float distance;
    float bend;
};

static struct flight flights[FLIGHT_POOL];

/* Filled by the event listener, drained on the display thread */
K_MSGQ_DEFINE(key_flight_queue, sizeof(char), 16, 1);

static uint32_t rng_state = 1;

static uint32_t rng_next(void) {
    /* xorshift32: plenty for picking paths */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static float rng_range(float lo, float hi) {
    return lo + (hi - lo) * (float)(rng_next() >> 8) * (1.0f / 16777216.0f);
}

struct punctuation {
    uint8_t usage;
    char plain;
    char shifted;
};

static const struct punctuation punctuation[] = {
    {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'},  {0x30, ']', '}'},
    {0x31, '\\', '|'}, {0x33, ';', ':'}, {0x34, '\'', '"'}, {0x35, '`', '~'},
    {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
    /* Keypad */
    {0x54, '/', '/'}, {0x55, '*', '*'}, {0x56, '-', '-'}, {0x57, '+', '+'}, {0x63, '.', '.'},
};

/* The character a keyboard-page usage types, or 0 for keys that do not fly */
static char usage_to_char(uint32_t usage, bool shift) {
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";

    if (usage >= 0x04 && usage <= 0x1D) {
        /* Letters always fly as capitals; the fonts hold no lowercase */
        return 'A' + (usage - 0x04);
    }
    if (usage >= 0x1E && usage <= 0x27) {
        return (shift ? shifted_digits : digits)[usage - 0x1E];
    }
    if (usage >= 0x59 && usage <= 0x62) {
        return digits[usage - 0x59];
    }
    for (size_t i = 0; i < ARRAY_SIZE(punctuation); i++) {
        if (punctuation[i].usage == usage) {
            return shift ? punctuation[i].shifted : punctuation[i].plain;
        }
    }
    return 0;
}

static void flight_place(struct flight *f, float u) {
    /* Accelerate toward the viewer: depth closes slowly, then rushes past */
    const float e = u * u;
    const float z = 1.0f - (1.0f - NEAREST_Z) * e;

    /* Travel along the heading, bowed sideways by the bend, seen through perspective */
    const float side = sinf(PI_F * u) * f->bend;
    const float wx = f->ux * f->distance * e - f->uy * side;
    const float wy = f->uy * f->distance * e + f->ux * side;
    const float sx = f->ox + wx / z;
    const float sy = f->oy + wy / z;

    /* The largest font step that does not overshoot the perspective size */
    const float size = START_SIZE / z;
    uint8_t font = 0;
    while (font + 1 < FLIGHT_FONT_COUNT && flight_font_px[font + 1] <= size) {
        font++;
    }
    if (font != f->font) {
        lv_obj_set_style_text_font(f->label, flight_fonts[font], LV_PART_MAIN);
        f->font = font;
    }

    /* Quick fade in, then fade out as it passes the viewer */
    const float fade_in = u * 8.0f < 1.0f ? u * 8.0f : 1.0f;
    const float alpha = fade_in * powf(1.0f - u, 1.3f);
    lv_obj_set_style_text_opa(f->label, (lv_opa_t)(alpha * 255.0f), LV_PART_MAIN);

    lv_obj_align(f->label, LV_ALIGN_CENTER, (int32_t)lroundf(sx - SCREEN_WIDTH / 2.0f),
                 (int32_t)lroundf(sy - SCREEN_HEIGHT / 2.0f));
}

static void flight_retire(struct flight *f) {
    f->active = false;
    lv_obj_add_flag(f->label, LV_OBJ_FLAG_HIDDEN);
}

static void flight_launch(char c, uint32_t now) {
    /* The first idle slot, or the oldest key in flight when none is idle */
    struct flight *f = NULL;
    for (int i = 0; i < FLIGHT_POOL; i++) {
        if (!flights[i].active) {
            f = &flights[i];
            break;
        }
        if (f == NULL || (int32_t)(flights[i].start - f->start) < 0) {
            f = &flights[i];
        }
    }

    const char text[2] = {c, '\0'};
    lv_label_set_text(f->label, text);

    const float heading = rng_range(0.0f, 2.0f * PI_F);
    f->ux = cosf(heading);
    f->uy = sinf(heading);
    f->distance = rng_range(80.0f, 130.0f);
    f->bend = rng_range(20.0f, 70.0f) * ((rng_next() & 1) ? 1.0f : -1.0f);
    /* Anywhere on screen, clear of the edges and of the indicators along the bottom */
    f->ox = rng_range(24.0f, SCREEN_WIDTH - 24.0f);
    f->oy = rng_range(16.0f, SCREEN_HEIGHT - 32.0f);
    f->start = now;
    f->duration = (uint32_t)rng_range(900.0f, 1200.0f);
    f->active = true;

    /* The newest key is the farthest away, so it draws beneath those already in flight */
    lv_obj_move_background(f->label);
    lv_obj_remove_flag(f->label, LV_OBJ_FLAG_HIDDEN);
    flight_place(f, 0.0f);
}

static void flight_frame_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    const uint32_t now = lv_tick_get();

    char c;
    while (k_msgq_get(&key_flight_queue, &c, K_NO_WAIT) == 0) {
        flight_launch(c, now);
    }

    for (int i = 0; i < FLIGHT_POOL; i++) {
        struct flight *f = &flights[i];
        if (!f->active) {
            continue;
        }
        const float u = (float)(now - f->start) / (float)f->duration;
        if (u >= 1.0f) {
            flight_retire(f);
        } else {
            flight_place(f, u);
        }
    }
}

static int key_flight_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state || ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t mods = ev->implicit_modifiers | zmk_hid_get_explicit_mods();
    const char c = usage_to_char(ev->keycode, (mods & (MOD_LSFT | MOD_RSFT)) != 0);
    if (c) {
        /* A full queue drops the key rather than hold up typing */
        k_msgq_put(&key_flight_queue, &c, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gossip_key_flight, key_flight_listener);
ZMK_SUBSCRIPTION(gossip_key_flight, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
/* Types a sentence on a loop, with a typist's uneven rhythm, so keys fly with nothing paired */
static const char demo_text[] = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG, 1234567890! ";
static uint32_t demo_next;
static size_t demo_pos;

static void flight_demo_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    const uint32_t now = lv_tick_get();
    if ((int32_t)(now - demo_next) < 0) {
        return;
    }
    const char c = demo_text[demo_pos];
    demo_pos = (demo_pos + 1) % (sizeof(demo_text) - 1);
    if (c != ' ') {
        k_msgq_put(&key_flight_queue, &c, K_NO_WAIT);
    }
    demo_next = now + (uint32_t)rng_range(70.0f, 230.0f) + ((rng_next() & 15) == 0 ? 600 : 0);
}
#endif

int zmk_widget_key_flight_init(lv_obj_t *parent) {
    rng_state = k_cycle_get_32() | 1;

    for (int i = 0; i < FLIGHT_POOL; i++) {
        struct flight *f = &flights[i];
        f->label = lv_label_create(parent);
        lv_obj_set_style_text_color(f->label, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(f->label, flight_fonts[0], LV_PART_MAIN);
        f->font = 0;
        lv_obj_add_flag(f->label, LV_OBJ_FLAG_HIDDEN);
        f->active = false;
    }

    lv_timer_create(flight_frame_cb, FRAME_MS, NULL);
#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
    lv_timer_create(flight_demo_cb, 20, NULL);
#endif
    return 0;
}
