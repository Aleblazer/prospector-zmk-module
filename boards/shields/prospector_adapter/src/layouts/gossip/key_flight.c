#include "key_flight.h"

#include <math.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
/*
 * LVGL 9.3 documents setting custom callbacks on the struct that
 * lv_draw_buf_get_handlers() returns, but keeps its fields in this header
 */
#include <src/draw/lv_draw_buf_private.h>

#include <fonts.h>

/*
 * Every printable key pressed on the keyboard appears at a random spot on
 * the screen and flies out toward the viewer along a random bent path:
 * growing as it nears, drifting off its heading and fading as it passes.
 *
 * Perspective is faked with a stepped set of font sizes rather than LVGL's
 * transform scaling, which is slow on this chip and blurs text. Each key is
 * tilted and spun with LVGL's transform rotation, which draws the label
 * into a temporary buffer first; those buffers come from a heap of their
 * own (see below). Keys are queued by the event listener and launched from
 * an LVGL timer, so all LVGL calls stay on the display thread.
 */

#define SCREEN_WIDTH 428
#define SCREEN_HEIGHT 142

/* Keys in flight at once; a new key takes over the oldest when all are busy */
#define FLIGHT_POOL 12
#define FRAME_MS 30

/*
 * A key grows from 12 px to 72 px, reaching full size 85% of the way
 * through its flight. The growth is exponential, so each frame scales it
 * by a similar factor, and bent by GROWTH_CURVE so the key lingers small
 * and then rushes through the large sizes: under 23 px for the first half,
 * then 28 px to 72 px in the last third.
 */
#define START_SIZE 12.0f
#define MAX_SIZE 72.0f
#define MAX_SIZE_AT 0.85f
#define GROWTH_CURVE 1.8f

/* Each key starts tilted up to this far either way and turns up to SPIN_MAX over its flight */
#define TILT_MAX_DEG 20.0f
#define SPIN_MAX_DEG 30.0f

/* Fully opaque until this far through the flight, then fading out */
#define FADE_START 0.74f

#define PI_F 3.14159265f

/*
 * About 15% apart up to 32 px and about 6% apart above. The growth curve
 * moves fastest through the large sizes, where each jump is also the most
 * pixels, so that is where the extra steps go; at 15% apart there the
 * jumps showed. Small keys grow slowly and their steps are only a pixel
 * or two.
 */
static const lv_font_t *const flight_fonts[] = {
    &DINish_SemiBold_12, &DINish_SemiBold_14, &DINish_SemiBold_16, &DINish_SemiBold_18,
    &DINish_SemiBold_21, &DINish_SemiBold_24, &DINish_SemiBold_28, &DINish_SemiBold_32,
    &DINish_SemiBold_34, &DINish_SemiBold_36, &DINish_SemiBold_38, &DINish_SemiBold_40,
    &DINish_SemiBold_43, &DINish_SemiBold_45, &DINish_SemiBold_48, &DINish_SemiBold_51,
    &DINish_SemiBold_54, &DINish_SemiBold_57, &DINish_SemiBold_61, &DINish_SemiBold_65,
    &DINish_SemiBold_68, &DINish_SemiBold_72,
};
static const uint8_t flight_font_px[] = {
    12, 14, 16, 18, 21, 24, 28, 32, 34, 36, 38, 40, 43, 45, 48, 51, 54, 57, 61, 65, 68, 72,
};
#define FLIGHT_FONT_COUNT ARRAY_SIZE(flight_fonts)

struct flight {
    lv_obj_t *label;
    bool active;
    uint8_t font;
    /* The key's character, shown with lv_label_set_text_static so launching allocates nothing */
    char text[2];
    uint32_t start;
    uint32_t duration;
    /* Launch point on screen, heading, how far it travels and how far its path bends */
    float ox, oy;
    float ux, uy;
    float distance;
    float bend;
    /* Starting angle and how far it turns over the flight, in degrees */
    float tilt;
    float spin;
};

static struct flight flights[FLIGHT_POOL];

/*
 * Temporary drawing buffers: the ARGB8888 layer each rotated key is drawn
 * into, about 17 KB for a 72 px key, and the per-letter buffers labels
 * use. LVGL takes these from its main pool by default, and when one does
 * not fit it waits and retries rather than failing. Among the labels,
 * texts and styles in that pool the free space fragmented until no gap
 * could hold a rotated key, and the display froze a couple of seconds
 * into typing. In a heap of their own they are all freed by the end of
 * each frame, so the heap empties completely and any one buffer fits.
 */
#define DRAW_HEAP_SIZE (40 * 1024)
K_HEAP_DEFINE(gossip_draw_heap, DRAW_HEAP_SIZE);

static bool in_draw_heap(const void *p) {
    const uintptr_t base = (uintptr_t)gossip_draw_heap.heap.init_mem;
    return (uintptr_t)p >= base && (uintptr_t)p < base + gossip_draw_heap.heap.init_bytes;
}

static void *draw_heap_malloc(size_t size, lv_color_format_t cf) {
    ARG_UNUSED(cf);
    /* Room to align the start, as LVGL's own allocator allows */
    return k_heap_alloc(&gossip_draw_heap, size + LV_DRAW_BUF_ALIGN - 1, K_NO_WAIT);
}

static void draw_heap_free(void *buf) {
    /* Anything allocated before the switch came from LVGL's pool */
    if (in_draw_heap(buf)) {
        k_heap_free(&gossip_draw_heap, buf);
    } else {
        lv_free(buf);
    }
}

static void draw_heap_install(void) {
    lv_draw_buf_handlers_t *handlers[] = {lv_draw_buf_get_handlers(),
                                          lv_draw_buf_get_font_handlers()};
    for (size_t i = 0; i < ARRAY_SIZE(handlers); i++) {
        handlers[i]->buf_malloc_cb = draw_heap_malloc;
        handlers[i]->buf_free_cb = draw_heap_free;
    }
}

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

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void flight_place(struct flight *f, float u) {
    /* Exponential growth along a bent timeline, holding at full size once reached */
    const float grown = powf(clampf(u / MAX_SIZE_AT, 0.0f, 1.0f), GROWTH_CURVE);
    const float size = START_SIZE * powf(MAX_SIZE / START_SIZE, grown);

    /* The largest font step that does not overshoot that size */
    uint8_t font = 0;
    while (font + 1 < FLIGHT_FONT_COUNT && flight_font_px[font + 1] <= size) {
        font++;
    }
    if (font != f->font) {
        lv_obj_set_style_text_font(f->label, flight_fonts[font], LV_PART_MAIN);
        f->font = font;
    }

    /* A gentle drift along the heading, bowed sideways by the bend */
    const float side = sinf(PI_F * u) * f->bend;
    float sx = f->ox + f->ux * f->distance * u - f->uy * side;
    float sy = f->oy + f->uy * f->distance * u + f->ux * side;

    /*
     * Keep the whole key on screen at its current size, so a key near an
     * edge is pushed inward as it grows instead of leaving the screen. The
     * half extents are rough: DINish capitals are about 0.7 of the font
     * size wide at most and the label box is about 0.9 tall.
     */
    const float half_w = 0.35f * flight_font_px[font];
    const float half_h = 0.45f * flight_font_px[font];
    sx = clampf(sx, half_w, SCREEN_WIDTH - half_w);
    sy = clampf(sy, half_h, SCREEN_HEIGHT - half_h);

    /* Quick fade in, fully opaque through most of the flight, then a smooth fade out */
    const float fade_in = clampf(u * 12.0f, 0.0f, 1.0f);
    const float t = clampf((u - FADE_START) / (1.0f - FADE_START), 0.0f, 1.0f);
    const float alpha = fade_in * (1.0f - t * t * (3.0f - 2.0f * t));
    lv_obj_set_style_text_opa(f->label, (lv_opa_t)(alpha * 255.0f), LV_PART_MAIN);

    /* LVGL rotation is in tenths of a degree, about the label's centre */
    lv_obj_set_style_transform_rotation(f->label, (int32_t)lroundf((f->tilt + f->spin * u) * 10.0f),
                                        LV_PART_MAIN);

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

    f->text[0] = c;
    f->text[1] = '\0';
    lv_label_set_text_static(f->label, f->text);

    const float heading = rng_range(0.0f, 2.0f * PI_F);
    f->ux = cosf(heading);
    f->uy = sinf(heading);
    f->distance = rng_range(30.0f, 70.0f);
    f->bend = rng_range(10.0f, 30.0f) * ((rng_next() & 1) ? 1.0f : -1.0f);
    f->tilt = rng_range(-TILT_MAX_DEG, TILT_MAX_DEG);
    f->spin = rng_range(-SPIN_MAX_DEG, SPIN_MAX_DEG);
    /* Anywhere on screen, clear of the edges and of the indicators along the bottom */
    f->ox = rng_range(24.0f, SCREEN_WIDTH - 24.0f);
    f->oy = rng_range(16.0f, SCREEN_HEIGHT - 32.0f);
    f->start = now;
    f->duration = (uint32_t)rng_range(900.0f, 1170.0f);
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
    draw_heap_install();

    for (int i = 0; i < FLIGHT_POOL; i++) {
        struct flight *f = &flights[i];
        f->label = lv_label_create(parent);
        /*
         * Set every style property a flight changes now, so later updates
         * overwrite values in place instead of growing the label's style
         * list in LVGL's pool while keys fly.
         */
        lv_obj_set_style_text_color(f->label, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(f->label, flight_fonts[0], LV_PART_MAIN);
        lv_obj_set_style_text_opa(f->label, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(f->label, 0, LV_PART_MAIN);
        /* Rotate about the centre, whatever size the key is at */
        lv_obj_set_style_transform_pivot_x(f->label, lv_pct(50), LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(f->label, lv_pct(50), LV_PART_MAIN);
        lv_obj_align(f->label, LV_ALIGN_CENTER, 0, 0);
        f->text[0] = '\0';
        lv_label_set_text_static(f->label, f->text);
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
