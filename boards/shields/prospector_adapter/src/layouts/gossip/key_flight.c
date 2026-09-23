#include "key_flight.h"

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>

#include <fonts.h>

#if IS_ENABLED(CONFIG_PROSPECTOR_GOSSIP_STATS)
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_heap.h>
#include <lvgl_mem.h>
LOG_MODULE_REGISTER(gossip, LOG_LEVEL_INF);
#endif

/*
 * Every printable key pressed on the keyboard appears at a random spot on
 * the screen and flies out toward the viewer along a random bent path:
 * growing as it nears, tilting and turning, drifting off its heading and
 * fading as it passes.
 *
 * Keys are drawn here rather than by LVGL. Each frame, every key in flight
 * is rendered once into its own A8 (alpha only) image at its exact size and
 * angle, sampled from a master glyph, and LVGL blends that image as a single
 * tinted fill. LVGL's transform rotation drew each rotated label into a full
 * colour layer first; with 4-5 keys it managed 10-21 frames a second and
 * filled 40 KB with layers until it crashed. Rendering here also makes the
 * growth continuous instead of stepping through font sizes.
 *
 * Keys are queued by the event listener and launched from an LVGL timer, so
 * all LVGL calls stay on the display thread.
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

#define KEY_COLOR 0xffffff

#define PI_F 3.14159265f

/*
 * Master glyphs, 8 bpp so a sample is one byte. A key is sampled from the
 * smallest master at least its size, so no master is ever shrunk by more
 * than half and bilinear sampling stays clean.
 */
static const lv_font_t *const masters[] = {
    &DINish_SemiBold_A8_18,
    &DINish_SemiBold_A8_36,
    &DINish_SemiBold_A8_72,
};
static const float master_px[] = {18.0f, 36.0f, 72.0f};
#define MASTER_COUNT ARRAY_SIZE(masters)

/*
 * Each key's image buffer is square, sized for its largest master glyph at
 * any angle: 4.2 KB for a typical character and 7.7 KB at most. They come
 * from this heap, which fits about 11 typical keys; when it is full, the
 * oldest keys give way to a new one. Nothing else allocates here, and every
 * buffer is freed when its key lands, so there is nothing to fragment it.
 */
#define KEY_HEAP_SIZE (48 * 1024)
K_HEAP_DEFINE(gossip_key_heap, KEY_HEAP_SIZE);

struct glyph {
    const uint8_t *bitmap;
    uint16_t w;
    uint16_t h;
};

struct flight {
    lv_obj_t *image;
    lv_image_dsc_t dsc;
    uint8_t *buf;
    uint32_t buf_size;
    struct glyph glyphs[MASTER_COUNT];
    bool active;
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

/* Filled by the event listener, drained on the display thread */
K_MSGQ_DEFINE(key_flight_queue, sizeof(char), 16, 1);

#if IS_ENABLED(CONFIG_PROSPECTOR_GOSSIP_STATS)
/*
 * Diagnostic: once a second, log the frame rate the animation actually
 * gets, the longest gap between frames, how many keys are in flight, were
 * dropped from a full queue or cut short to free memory, and how full the
 * key heap and LVGL's pool are.
 */
static atomic_t stat_dropped;
static uint32_t stat_cut_short;
static uint32_t stat_frames;
static uint32_t stat_worst_gap;
static uint32_t stat_last_frame;
static uint32_t stat_window_start;

static void stats_frame(uint32_t now, int in_flight) {
    if (stat_last_frame != 0 && now - stat_last_frame > stat_worst_gap) {
        stat_worst_gap = now - stat_last_frame;
    }
    stat_last_frame = now;
    stat_frames++;

    if (now - stat_window_start < 1000) {
        return;
    }

    struct sys_memory_stats keys;
    struct sys_memory_stats pool;
    sys_heap_runtime_stats_get(&gossip_key_heap.heap, &keys);
    lvgl_heap_stats(&pool);

    LOG_INF("fps %u, worst gap %u ms, in flight %d, dropped %d, cut short %u | key heap used %u "
            "peak %u free %u | lvgl pool used %u peak %u free %u",
            stat_frames * 1000 / (now - stat_window_start), stat_worst_gap, in_flight,
            (int)atomic_get(&stat_dropped), stat_cut_short, keys.allocated_bytes,
            keys.max_allocated_bytes, keys.free_bytes, pool.allocated_bytes,
            pool.max_allocated_bytes, pool.free_bytes);

    stat_frames = 0;
    stat_worst_gap = 0;
    stat_window_start = now;
}
#endif

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

/* A master's 8 bpp bitmap for a character: box_w x box_h bytes, row by row */
static bool glyph_get(const lv_font_t *font, char c, struct glyph *out) {
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)c, 0) || g.box_w == 0 || g.box_h == 0) {
        return false;
    }
    const lv_font_fmt_txt_dsc_t *fdsc = font->dsc;
    out->bitmap = &fdsc->glyph_bitmap[fdsc->glyph_dsc[g.gid.index].bitmap_index];
    out->w = g.box_w;
    out->h = g.box_h;
    return true;
}

static inline float glyph_px(const struct glyph *g, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return 0.0f;
    }
    return g->bitmap[y * g->w + x];
}

/* Bilinear sample at glyph coordinates, where pixel centres are whole numbers */
static inline uint8_t glyph_sample(const struct glyph *g, float u, float v) {
    if (u <= -1.0f || v <= -1.0f || u >= g->w || v >= g->h) {
        return 0;
    }
    const float fx = floorf(u);
    const float fy = floorf(v);
    const int32_t x = (int32_t)fx;
    const int32_t y = (int32_t)fy;
    const float ax = u - fx;
    const float ay = v - fy;
    const float top = glyph_px(g, x, y) + (glyph_px(g, x + 1, y) - glyph_px(g, x, y)) * ax;
    const float bottom =
        glyph_px(g, x, y + 1) + (glyph_px(g, x + 1, y + 1) - glyph_px(g, x, y + 1)) * ax;
    return (uint8_t)(top + (bottom - top) * ay + 0.5f);
}

static void flight_retire(struct flight *f) {
    f->active = false;
    lv_obj_add_flag(f->image, LV_OBJ_FLAG_HIDDEN);
    if (f->buf != NULL) {
        k_heap_free(&gossip_key_heap, f->buf);
        f->buf = NULL;
        f->buf_size = 0;
    }
}

static void flight_place(struct flight *f, float u) {
    /* Exponential growth along a bent timeline, holding at full size once reached */
    const float grown = powf(clampf(u / MAX_SIZE_AT, 0.0f, 1.0f), GROWTH_CURVE);
    const float size = START_SIZE * powf(MAX_SIZE / START_SIZE, grown);

    /* The smallest master at least this size, and how much to shrink it */
    uint8_t m = 0;
    while (m + 1 < MASTER_COUNT && master_px[m] < size) {
        m++;
    }
    const struct glyph *g = &f->glyphs[m];
    const float scale = size / master_px[m];
    const float inv_scale = 1.0f / scale;

    const float angle = (f->tilt + f->spin * u) * (PI_F / 180.0f);
    const float c = cosf(angle);
    const float s = sinf(angle);

    /* Half extents of the scaled, rotated glyph box on screen */
    const float hw = 0.5f * scale * g->w;
    const float hh = 0.5f * scale * g->h;
    const float ex = fabsf(c) * hw + fabsf(s) * hh;
    const float ey = fabsf(s) * hw + fabsf(c) * hh;

    /*
     * A gentle drift along the heading, bowed sideways by the bend, held
     * inside the screen so a key near an edge is pushed inward as it grows.
     */
    const float side = sinf(PI_F * u) * f->bend;
    const float sx = clampf(f->ox + f->ux * f->distance * u - f->uy * side, ex, SCREEN_WIDTH - ex);
    const float sy = clampf(f->oy + f->uy * f->distance * u + f->ux * side, ey, SCREEN_HEIGHT - ey);

    /* The image covers the rotated box plus a pixel for the bilinear edge */
    const int32_t x0 = (int32_t)floorf(sx - ex) - 1;
    const int32_t y0 = (int32_t)floorf(sy - ey) - 1;
    const int32_t w = (int32_t)ceilf(sx + ex) + 1 - x0;
    const int32_t h = (int32_t)ceilf(sy + ey) + 1 - y0;
    const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_A8);
    if (w <= 0 || h <= 0 || stride * h > f->buf_size) {
        return;
    }

    /*
     * Walk the image, mapping each pixel centre back into the master glyph:
     * undo the rotation, then the scale. Along a row the glyph position
     * moves by a constant step.
     */
    const float gcx = 0.5f * g->w - 0.5f;
    const float gcy = 0.5f * g->h - 0.5f;
    const float du = c * inv_scale;
    const float dv = -s * inv_scale;
    for (int32_t y = 0; y < h; y++) {
        const float py = (float)(y0 + y) + 0.5f - sy;
        const float px = (float)x0 + 0.5f - sx;
        float gu = (c * px + s * py) * inv_scale + gcx;
        float gv = (-s * px + c * py) * inv_scale + gcy;
        uint8_t *row = f->buf + y * stride;
        for (int32_t x = 0; x < w; x++) {
            row[x] = glyph_sample(g, gu, gv);
            gu += du;
            gv += dv;
        }
    }

    f->dsc.header.w = w;
    f->dsc.header.h = h;
    f->dsc.header.stride = stride;
    f->dsc.data_size = stride * h;
    lv_image_set_src(f->image, &f->dsc);
    lv_obj_set_pos(f->image, x0, y0);

    /* Quick fade in, fully opaque through most of the flight, then a smooth fade out */
    const float fade_in = clampf(u * 12.0f, 0.0f, 1.0f);
    const float t = clampf((u - FADE_START) / (1.0f - FADE_START), 0.0f, 1.0f);
    const float alpha = fade_in * (1.0f - t * t * (3.0f - 2.0f * t));
    lv_obj_set_style_image_opa(f->image, (lv_opa_t)(alpha * 255.0f), LV_PART_MAIN);
}

static struct flight *flight_oldest(const struct flight *except) {
    struct flight *oldest = NULL;
    for (int i = 0; i < FLIGHT_POOL; i++) {
        struct flight *f = &flights[i];
        if (f == except || !f->active) {
            continue;
        }
        if (oldest == NULL || (int32_t)(f->start - oldest->start) < 0) {
            oldest = f;
        }
    }
    return oldest;
}

static void flight_launch(char c, uint32_t now) {
    struct glyph glyphs[MASTER_COUNT];
    for (size_t i = 0; i < MASTER_COUNT; i++) {
        if (!glyph_get(masters[i], c, &glyphs[i])) {
            return;
        }
    }

    /* The first idle slot, or the oldest key in flight when none is idle */
    struct flight *f = NULL;
    for (int i = 0; i < FLIGHT_POOL && f == NULL; i++) {
        if (!flights[i].active) {
            f = &flights[i];
        }
    }
    if (f == NULL) {
        f = flight_oldest(NULL);
        flight_retire(f);
    }

    /* Square room for the largest master glyph at any angle, plus bilinear edges */
    const struct glyph *big = &glyphs[MASTER_COUNT - 1];
    const uint32_t side = (uint32_t)ceilf(sqrtf((float)(big->w * big->w + big->h * big->h))) + 4;
    const uint32_t bytes = lv_draw_buf_width_to_stride(side, LV_COLOR_FORMAT_A8) * side;
    const size_t align = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);

    uint8_t *buf = k_heap_aligned_alloc(&gossip_key_heap, align, bytes, K_NO_WAIT);
    while (buf == NULL) {
        /* Cut the oldest key short to make room; with none left, skip this one */
        struct flight *oldest = flight_oldest(f);
        if (oldest == NULL) {
            return;
        }
        flight_retire(oldest);
#if IS_ENABLED(CONFIG_PROSPECTOR_GOSSIP_STATS)
        stat_cut_short++;
#endif
        buf = k_heap_aligned_alloc(&gossip_key_heap, align, bytes, K_NO_WAIT);
    }

    f->buf = buf;
    f->buf_size = bytes;
    f->dsc.data = buf;
    memcpy(f->glyphs, glyphs, sizeof(glyphs));

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
    lv_obj_move_background(f->image);
    flight_place(f, 0.0f);
    lv_obj_remove_flag(f->image, LV_OBJ_FLAG_HIDDEN);
}

static void flight_frame_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    const uint32_t now = lv_tick_get();

    char c;
    while (k_msgq_get(&key_flight_queue, &c, K_NO_WAIT) == 0) {
        flight_launch(c, now);
    }

    int in_flight = 0;
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
            in_flight++;
        }
    }

#if IS_ENABLED(CONFIG_PROSPECTOR_GOSSIP_STATS)
    stats_frame(now, in_flight);
#else
    ARG_UNUSED(in_flight);
#endif
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
        if (k_msgq_put(&key_flight_queue, &c, K_NO_WAIT) != 0) {
#if IS_ENABLED(CONFIG_PROSPECTOR_GOSSIP_STATS)
            atomic_inc(&stat_dropped);
#endif
        }
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
        f->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        f->dsc.header.cf = LV_COLOR_FORMAT_A8;

        f->image = lv_image_create(parent);
        /*
         * An A8 image is drawn as a fill in its recolour, with image_opa as
         * the fade. Every style property a flight changes is set now, so
         * later updates overwrite values in place instead of growing the
         * image's style list in LVGL's pool while keys fly.
         */
        lv_obj_set_style_image_recolor(f->image, lv_color_hex(KEY_COLOR), LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(f->image, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_image_opa(f->image, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_pos(f->image, 0, 0);
        lv_obj_add_flag(f->image, LV_OBJ_FLAG_HIDDEN);
        f->active = false;
    }

    lv_timer_create(flight_frame_cb, FRAME_MS, NULL);
#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
    lv_timer_create(flight_demo_cb, 20, NULL);
#endif
    return 0;
}
