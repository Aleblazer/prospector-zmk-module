#include "bubbledash.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <zmk/activity.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_central_status_changed.h>
#include <zmk/keymap.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#include <fonts.h>

/*
 * BubbleDash: the hero runs along a parallax landscape at a speed set by
 * your typing. Typing makes him hop, spin, flip and long-jump at random;
 * past 85 WPM he chains three jumps, each higher than the last.
 *
 * The game advances in fixed 60 Hz steps, so it plays at the same speed
 * whatever the frame rate; each frame is then drawn in 16-row strips and
 * written straight to the panel.
 */

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint16_t strip_buf[BD_W * BD_STRIP_H];

#define STEP_MS (1000.0f / 60.0f)
#define FRAME_MS_MOVING 33
#define FRAME_MS_STILL 50
#define FRAME_MS_IDLE 200
#define FIRST_FRAME_DELAY_MS 500

/* --- Input -------------------------------------------------------------- */

static atomic_t pending_keys;

#define KEY_LOG 64
static int64_t key_times[KEY_LOG];
static int key_head, key_count;

/* A small xorshift: plenty for picking jumps */
static uint32_t rng_state = 0x9e3779b9;
static float rnd(float a, float b) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return a + (b - a) * (rng_state * (1.0f / 4294967296.0f));
}

/* --- Simulation ---------------------------------------------------------- */

enum jump {
    J_HOP,
    J_HIGH,
    J_SPIN,
    J_LONG,
    J_FLIP,
    J_C1,
    J_C2,
    J_C3,
};

enum gait { G_IDLE, G_WALK, G_RUN, G_SPRINT };

#define GRAVITY 0.45f
#define BD_PI 3.14159265f
static const float jump_speed[] = {3.8f, 5.4f, 4.8f, 3.6f, 5.2f, 4.2f, 5.2f, 6.3f};

static double scroll;
static float speed, boost, phase, wpm;
static uint32_t tick;
static enum gait gait = G_IDLE;
static int banner;

static struct {
    float y, vy;
    bool air;
    enum jump kind;
    int t, land, stage, chain;
} hero;

enum particle_kind { P_DUST, P_SPARK, P_LINE };

#define MAX_PARTICLES 48
static struct particle {
    float x, y, vx, vy;
    int16_t life, max;
    uint8_t kind;
} particles[MAX_PARTICLES];
static int particle_count;

static void emit(float x, float y, float vx, float vy, int life, enum particle_kind kind) {
    if (particle_count < MAX_PARTICLES) {
        particles[particle_count++] = (struct particle){x, y, vx, vy, life, life, kind};
    }
}

static void jump(enum jump k) {
    hero.air = true;
    hero.kind = k;
    hero.vy = jump_speed[k];
    hero.t = 0;
    if (k == J_LONG) {
        boost = 2.6f;
    }
    if (k == J_C3) {
        banner = 80;
    }
}

static void on_key(int64_t now) {
    key_times[key_head] = now;
    key_head = (key_head + 1) % KEY_LOG;
    key_count = MIN(key_count + 1, KEY_LOG);

    if (hero.air || hero.land > 0) {
        return;
    }
    if (wpm >= 85) {
        if (rnd(0, 1) < 0.35f) {
            hero.stage = 1;
            jump(J_C1);
        }
        return;
    }
    if (rnd(0, 1) < 0.14f + wpm / 320) {
        static const enum jump picks[] = {J_HOP, J_HOP, J_HIGH, J_SPIN, J_LONG, J_FLIP};
        hero.stage = 0;
        jump(picks[(int)rnd(0, ARRAY_SIZE(picks)) % ARRAY_SIZE(picks)]);
    }
}

/* Keys in the last four seconds, times three: words of five a minute */
static int recent_keys(int64_t now) {
    int n = 0;
    for (int i = 0; i < key_count; i++) {
        if (now - key_times[(key_head - 1 - i + KEY_LOG) % KEY_LOG] < 4000) {
            n++;
        } else {
            break;
        }
    }
    return n;
}

static void step(int64_t now) {
    tick++;
    for (int n = atomic_set(&pending_keys, 0); n > 0; n--) {
        on_key(now);
    }
    wpm += (recent_keys(now) * 3 - wpm) * 0.04f;

    const float target = wpm < 3 ? 0 : wpm * 0.026f;
    speed += (target - speed) * 0.05f;
    boost *= 0.95f;
    const float sp = speed + boost;
    scroll += (double)sp;

    gait = sp < 0.1f ? G_IDLE : sp < 1 ? G_WALK : sp < 2 ? G_RUN : G_SPRINT;
    if (gait != G_IDLE) {
        phase += sp * 0.055f + 0.05f;
        if (phase > 2 * BD_PI) {
            phase -= 2 * BD_PI;
        }
    }

    if (hero.air) {
        hero.t++;
        hero.vy -= GRAVITY;
        hero.y += hero.vy;
        if (hero.y <= 0) {
            hero.y = 0;
            hero.air = false;
            hero.land = 6;
            for (int i = 0; i < 5; i++) {
                emit(BD_HX + rnd(-6, 6), BD_GY - 2, rnd(-1.8f, -0.2f), rnd(-0.8f, -0.1f), 22, P_DUST);
            }
            if (hero.stage > 0 && hero.stage < 3 && wpm >= 75) {
                hero.chain = 4;
            } else {
                if (hero.stage == 3) {
                    for (int i = 0; i < 14; i++) {
                        emit(BD_HX + rnd(-10, 10), BD_GY - 30, rnd(-1.6f, 1.6f), rnd(-2, 0.4f), 46,
                             P_SPARK);
                    }
                }
                hero.stage = 0;
            }
        }
    } else if (hero.land > 0) {
        hero.land--;
    }

    if (hero.chain > 0 && --hero.chain == 0) {
        hero.stage++;
        hero.land = 0;
        jump(hero.stage == 2 ? J_C2 : J_C3);
    }

    if (!hero.air && gait >= G_RUN && tick % (gait == G_SPRINT ? 5 : 9) == 0) {
        emit(BD_HX - 8, BD_GY - 2, -sp * 0.5f, -0.25f, 16, P_DUST);
    }
    if (gait == G_SPRINT && tick % 3 == 0) {
        emit(BD_HX - 26, BD_GY - 12 - rnd(0, 26), -6, 0, 10, P_LINE);
    }

    int kept = 0;
    for (int i = 0; i < particle_count; i++) {
        struct particle *q = &particles[i];
        q->x += q->vx;
        q->y += q->vy;
        if (q->kind == P_DUST) {
            q->vy -= 0.02f;
        } else if (q->kind == P_SPARK) {
            q->vy += 0.06f;
        }
        if (--q->life > 0) {
            particles[kept++] = *q;
        }
    }
    particle_count = kept;

    if (banner > 0) {
        banner--;
    }
}

/* --- Poses --------------------------------------------------------------- */

/*
 * Poses are written in the mockup's 96 px sprite space, whose ground sat on
 * row 88 and body centre on column 48; P() moves them into the 80 px one.
 */
#define OX (48 - BD_HERO_BX)
#define OY (88 - BD_HERO_GROUND)

static struct bd_pose pose;
static float leaf_x, leaf_y, leaf_vx, leaf_vy;
static bool leaf_ready;

static void feet(struct bd_pose *p, float fx, float fy, float bx, float by) {
    p->ffx = fx - OX;
    p->ffy = fy - OY;
    p->bfx = bx - OX;
    p->bfy = by - OY;
}

static void hands(struct bd_pose *p, float nx, float ny, float fx, float fy) {
    p->nearx = nx;
    p->neary = ny;
    p->farx = fx;
    p->fary = fy;
}

static void make_pose(struct bd_pose *p) {
    /* 62832 steps is almost exactly 600 turns of both 0.06 and 0.6, so the wrap is seamless */
    const float ft = (float)(tick % 62832);
    const float BR = BD_HERO_BR, BRY = BD_HERO_BRY, fl = ft * 0.6f;
    bool air = false;
    memset(p, 0, sizeof(*p));

    if (hero.air) {
        const enum jump k = hero.kind;
        air = true;
        p->mouth_open = true;
        if (k == J_FLIP || k == J_C3) {
            const float total = 2 * jump_speed[k] / GRAVITY;
            p->rx = BR - 1;
            p->ry = BRY - 1;
            p->rot = MIN(1.0f, hero.t / total) * 2 * BD_PI;
            feet(p, 53, 80, 44, 81);
            hands(p, 4, 6, 10, 5);
            p->mouth_open = false;
        } else if (k == J_LONG) {
            p->rx = BR + 3;
            p->ry = BRY - 2;
            p->tilt = 0.25f;
            feet(p, 66, 81, 34, 83);
            hands(p, 13, 1, 18, -3);
        } else if (hero.vy > 0 || k == J_HIGH || k == J_C2) {
            /* On the way up: near hand swept to the lower back, far hand punching up */
            const float s = MIN(1.0f, fabsf(hero.vy) / 5);
            p->rx = BR - 2 * s;
            p->ry = BRY + 2.5f * s;
            p->tilt = 0.05f;
            feet(p, 53, 81, 43, 82);
            hands(p, -15, 9, 10, -16);
            p->look = -1;
        } else {
            p->rx = BR + 1;
            p->ry = BRY - 0.5f;
            feet(p, 57, 85, 39, 85);
            hands(p, -7 + sinf(fl) * 3, -9, 13, -11 + sinf(fl + 1.5f) * 3);
        }
    } else if (hero.land > 0) {
        const float s = hero.land / 6.0f;
        p->rx = BR + 4 * s;
        p->ry = BRY - 5 * s;
        feet(p, 58, 85, 38, 85);
        hands(p, -1, 10, 11, 9);
    } else if (gait == G_IDLE) {
        const float b = sinf(ft * 0.06f);
        p->rx = BR + 0.4f * b;
        p->ry = BRY - 0.5f * b;
        feet(p, 55, 85, 41, 85);
        hands(p, -3, 7 + b * 0.5f, 10, 6);
        p->blink = (tick % 200) < 7;
    } else {
        static const float A[] = {0, 4, 7, 9}, HL[] = {0, 2, 4, 5}, BO[] = {0, 1.5f, 4, 5},
                           SQ[] = {0, 0.8f, 1.8f, 2}, TL[] = {0, 0.05f, 0.14f, 0.24f},
                           AK[] = {0, 1, 1.4f, 1.6f};
        const float s = sinf(phase), c = cosf(phase), ab = fabsf(s), lift = ab * BO[gait];
        const bool sprint = gait == G_SPRINT;
        p->rx = BR + (1 - ab) * SQ[gait] * 0.8f + (sprint ? 1.5f : 0);
        p->ry = BRY - (1 - ab) * SQ[gait] - (sprint ? 1 : 0);
        p->lift = lift;
        p->tilt = TL[gait];
        feet(p, 52 + A[gait] * s, 85 - MAX(0.0f, c) * HL[gait] - lift * 0.5f, 44 - A[gait] * s,
             85 - MAX(0.0f, -c) * HL[gait] - lift * 0.5f);
        hands(p, -s * 8 * AK[gait], 7 - 2 * ab, 4 + s * 10 * AK[gait], 6 - 2 * ab);
        p->mouth_open = sprint;
        p->look = sprint ? 1 : 0;
        p->blink = (tick % 230) < 6;
    }

    p->cx = 48 - OX;
    p->cy = 85 - OY - p->ry - p->lift - (air ? 3 : 0);
}

static int sprite_x(void) { return BD_HX - BD_HERO_BX; }
static int sprite_y(void) { return (int)lroundf(BD_GY - BD_HERO_GROUND - hero.y); }

/* The sprout trails behind on a damped spring, never more than 5 px off */
static void leaf_step(void) {
    float tx, ty;
    bd_hero_leaf_target(&pose, &tx, &ty);
    tx += sprite_x();
    ty += sprite_y();
    if (!leaf_ready) {
        leaf_x = tx;
        leaf_y = ty;
        leaf_ready = true;
    }
    leaf_vx = (leaf_vx + (tx - leaf_x) * 0.22f) * 0.72f;
    leaf_vy = (leaf_vy + (ty - leaf_y) * 0.22f) * 0.72f;
    leaf_x += leaf_vx;
    leaf_y += leaf_vy;
    const float dx = leaf_x - tx, dy = leaf_y - ty, d = sqrtf(dx * dx + dy * dy);
    if (d > 5) {
        leaf_x = tx + dx / d * 5;
        leaf_y = ty + dy / d * 5;
    }
}

/* --- Text and HUD -------------------------------------------------------- */

static char layer_name[24];

static const lv_font_fmt_txt_glyph_dsc_t *glyph(const lv_font_t *font, char ch) {
    const lv_font_fmt_txt_dsc_t *dsc = font->dsc;
    for (int i = 0; i < dsc->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *cm = &dsc->cmaps[i];
        const uint32_t ofs = (uint8_t)ch - cm->range_start;
        if (cm->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY && ofs < cm->range_length) {
            return &dsc->glyph_dsc[cm->glyph_id_start + ofs];
        }
    }
    return NULL;
}

static int text_width(const lv_font_t *font, const char *s) {
    int w = 0;
    for (; *s; s++) {
        const lv_font_fmt_txt_glyph_dsc_t *g = glyph(font, *s);
        w += g ? (g->adv_w + 8) >> 4 : 0;
    }
    return w;
}

/* Draws 4 bpp text with its line box's top left at (x, y), faded by alpha */
static void text(const lv_font_t *font, const char *s, int x, int y, uint32_t colour,
                 uint8_t alpha) {
    const lv_font_fmt_txt_dsc_t *dsc = font->dsc;
    const int baseline = y + font->line_height - font->base_line;
    for (; *s; s++) {
        const lv_font_fmt_txt_glyph_dsc_t *g = glyph(font, *s);
        if (g == NULL) {
            continue;
        }
        const uint8_t *bmp = &dsc->glyph_bitmap[g->bitmap_index];
        const int gx = x + g->ofs_x, gy = baseline - g->box_h - g->ofs_y;
        for (int r = 0; r < g->box_h; r++) {
            const int py = gy + r;
            if (py < bd_y0 || py >= bd_y1) {
                continue;
            }
            uint16_t *row = bd_row(py);
            for (int c = 0; c < g->box_w; c++) {
                const int k = r * g->box_w + c, px = gx + c;
                const uint8_t nib = (bmp[k >> 1] >> ((k & 1) ? 0 : 4)) & 0xF;
                if (nib && (unsigned)px < BD_W) {
                    row[px] = bd_blend(row[px], colour, (uint8_t)(nib * 17 * alpha / 255));
                }
            }
        }
        x += (g->adv_w + 8) >> 4;
    }
}

#ifndef PERIPHERAL_COUNT
#define PERIPHERAL_COUNT ZMK_SPLIT_BLE_PERIPHERAL_COUNT
#endif
#define BARS_SHOWN (PERIPHERAL_COUNT > 3 ? 3 : PERIPHERAL_COUNT)

struct bd_battery_state {
    uint8_t level[PERIPHERAL_COUNT];
    bool connected[PERIPHERAL_COUNT];
};
static struct bd_battery_state battery_table, battery_shown;

#define BAR_W 21
#define BAR_H 5
#define BAR_GAP 4

static void draw_hud(void) {
    text(&DINishCondensed_SemiBold_20, layer_name, 11, 7, 0x000000, 255);
    text(&DINishCondensed_SemiBold_20, layer_name, 10, 6, 0xffffff, 255);

    int bx = BD_W - 10 - BARS_SHOWN * BAR_W - (BARS_SHOWN - 1) * BAR_GAP;
    for (int i = 0; i < BARS_SHOWN; i++, bx += BAR_W + BAR_GAP) {
        const int lvl = battery_shown.connected[i] ? battery_shown.level[i] : 0;
        const int fill = (19 * lvl + 50) / 100;
        for (int y = MAX(bd_y0, 10); y < MIN(bd_y1, 10 + BAR_H); y++) {
            uint16_t *row = bd_row(y);
            for (int x = bx; x < bx + BAR_W; x++) {
                const bool inner = y > 10 && y < 10 + BAR_H - 1 && x > bx && x <= bx + fill;
                row[x] = inner ? bd_rgb(lvl < 20 ? 0xe0a020 : 0xffffff)
                               : bd_blend(row[x], 0x000000, 140);
            }
        }
    }

    if (banner > 0) {
        static const char word[] = "TRIPLE!";
        const lv_font_t *f = &DINishCondensed_SemiBold_28;
        const uint8_t a = (uint8_t)(MIN(20, banner) * 255 / 20);
        const int x = (BD_W - text_width(f, word)) / 2, y = 8;
        text(f, word, x + 1, y + 2, 0x173d12, a);
        text(f, word, x, y + 1, 0x4fa52c, a);
        text(f, word, x, y, 0xc9f38c, a);
    }
}

/* --- Frame --------------------------------------------------------------- */

static int shadow_r;
static float shadow_rf;
static int sox, soy;
static bool mirror;

static void draw_strip(void) {
    bd_world_draw_back();

    /* The hero's shadow shrinks as he rises */
    for (int dy = -2; dy <= 2; dy++) {
        const int y = BD_GY + 1 + dy;
        if (y < bd_y0 || y >= bd_y1) {
            continue;
        }
        uint16_t *row = bd_row(y);
        for (int dx = -shadow_r; dx <= shadow_rf; dx++) {
            const int x = BD_HX + dx;
            if (dx * dx / (shadow_rf * shadow_rf) + dy * dy / 6.25f <= 1 && (unsigned)x < BD_W) {
                row[x] = bd_darken(row[x]);
            }
        }
    }

    bd_hero_draw(sox, soy, mirror);

    for (int i = 0; i < particle_count; i++) {
        const struct particle *q = &particles[i];
        const float f = (float)q->life / q->max;
        const int x = (int)floorf(q->x), y = (int)floorf(q->y);
        if (q->kind == P_DUST) {
            bd_disc(q->x, q->y, 1 + f * 2.2f, bd_world_dust(), 1e9f);
        } else if (q->kind == P_SPARK) {
            const uint16_t c = q->life % 8 < 4 ? bd_hero_colour(4, 4) : bd_hero_colour(6, 4);
            bd_put(x, y, c);
            if (f > 0.4f) {
                static const int8_t arms[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                                 {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
                for (size_t k = 0; k < ARRAY_SIZE(arms); k++) {
                    const bool inner = abs(arms[k][0] + arms[k][1]) < 2;
                    bd_put(x + arms[k][0], y + arms[k][1],
                           f > 0.7f || inner ? c : bd_hero_colour(6, 3));
                }
            }
        } else {
            for (int k = 0; k < 12; k++) {
                bd_put(x + k, y, bd_hero_colour(1, k < 4 ? 4 : 3));
            }
        }
    }

    bd_world_draw_front();
    draw_hud();
}

static void render(void) {
    make_pose(&pose);
    sox = sprite_x();
    soy = sprite_y();
    mirror = hero.air && hero.kind == J_SPIN && (hero.t / 4) % 2 == 1;
    shadow_rf = (pose.rx + 2) * (1 - MIN(0.6f, hero.y / 70));
    shadow_r = (int)ceilf(shadow_rf);

    bd_world_prepare(scroll, tick);
    bd_hero_build(&pose, leaf_x - sox, leaf_y - soy);

    bd_strip = strip_buf;
    for (int y0 = 0; y0 < BD_H; y0 += BD_STRIP_H) {
        bd_y0 = y0;
        bd_y1 = MIN(BD_H, y0 + BD_STRIP_H);
        draw_strip();

        const int rows = bd_y1 - bd_y0;
        const struct display_buffer_descriptor desc = {
            .buf_size = BD_W * rows * sizeof(uint16_t),
            .width = BD_W,
            .height = rows,
            .pitch = BD_W,
            .frame_incomplete = bd_y1 < BD_H,
        };
        display_write(display, 0, y0, &desc, strip_buf);
    }
}

static struct k_work_delayable frame_work;
static int64_t last_ms;
static float pending_ms;

static void frame_handler(struct k_work *work) {
    const enum zmk_activity_state activity = zmk_activity_get_state();
    if (activity == ZMK_ACTIVITY_SLEEP) {
        /* Woken again by the activity listener */
        last_ms = 0;
        return;
    }

    const int64_t start = k_uptime_get();
    if (last_ms == 0) {
        last_ms = start;
    }
    pending_ms = MIN(100.0f, pending_ms + (float)(start - last_ms));
    last_ms = start;
    while (pending_ms >= STEP_MS) {
        pending_ms -= STEP_MS;
        step(start);
        make_pose(&pose);
        leaf_step();
    }

    render();

    const int period = activity == ZMK_ACTIVITY_IDLE ? FRAME_MS_IDLE
                       : gait == G_IDLE && !hero.air && particle_count == 0 && banner == 0
                           ? FRAME_MS_STILL
                           : FRAME_MS_MOVING;
    const int spent = (int)(k_uptime_get() - start);
    /* Always yield a little so LVGL's tick and the widget listeners get a turn */
    k_work_schedule_for_queue(zmk_display_work_q(), &frame_work, K_MSEC(MAX(2, period - spent)));
}

/* --- Listeners ----------------------------------------------------------- */

static int bd_key_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev != NULL && ev->state && ev->usage_page == HID_USAGE_KEY && ev->keycode < 0xE0) {
        atomic_inc(&pending_keys);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bubbledash_keys, bd_key_listener);
ZMK_SUBSCRIPTION(bubbledash_keys, zmk_keycode_state_changed);

static int bd_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev != NULL && ev->state == ZMK_ACTIVITY_ACTIVE) {
        k_work_reschedule_for_queue(zmk_display_work_q(), &frame_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bubbledash_activity, bd_activity_listener);
ZMK_SUBSCRIPTION(bubbledash_activity, zmk_activity_state_changed);

struct bd_layer_state {
    uint8_t index;
};

static void bd_layer_update_cb(struct bd_layer_state state) {
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(state.index));
    if (name && *name) {
        snprintf(layer_name, sizeof(layer_name), "%s", name);
    } else {
        snprintf(layer_name, sizeof(layer_name), "%d", state.index);
    }
    for (int i = 0; layer_name[i]; i++) {
        layer_name[i] = toupper((unsigned char)layer_name[i]);
    }
    bd_world_set_biome(state.index % BD_BIOME_COUNT);
}

static struct bd_layer_state bd_layer_get_state(const zmk_event_t *eh) {
    return (struct bd_layer_state){.index = zmk_keymap_highest_layer_active()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_bubbledash_layer, struct bd_layer_state, bd_layer_update_cb,
                            bd_layer_get_state)
ZMK_SUBSCRIPTION(widget_bubbledash_layer, zmk_layer_state_changed);

/*
 * Every peripheral's charge and link in one state: ZMK's display listener
 * keeps only the latest state, so per-event deltas would get lost.
 */
static void bd_battery_update_cb(struct bd_battery_state state) { battery_shown = state; }

static struct bd_battery_state bd_battery_get_state(const zmk_event_t *eh) {
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

ZMK_DISPLAY_WIDGET_LISTENER(widget_bubbledash_battery, struct bd_battery_state,
                            bd_battery_update_cb, bd_battery_get_state)
ZMK_SUBSCRIPTION(widget_bubbledash_battery, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(widget_bubbledash_battery, zmk_split_central_status_changed);

void bd_start(void) {
    rng_state ^= k_cycle_get_32();
    bd_hero_init();
    bd_world_set_biome(0);
    widget_bubbledash_layer_init();
    widget_bubbledash_battery_init();

    k_work_init_delayable(&frame_work, frame_handler);
    /* After LVGL has put up its black screen, which it then never redraws */
    k_work_schedule_for_queue(zmk_display_work_q(), &frame_work, K_MSEC(FIRST_FRAME_DELAY_MS));
}
