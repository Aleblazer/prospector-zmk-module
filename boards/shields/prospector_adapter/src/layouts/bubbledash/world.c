#include "bubbledash.h"

#include <math.h>
#include <zephyr/sys/util.h>

/*
 * The landscape behind and in front of the hero. Each layer scrolls at its
 * own fraction of the hero's speed: clouds 0.06, far mountains 0.08, near
 * mountains 0.16, hills and trees 0.32, bushes and fence 0.6, ground 1 and
 * foreground grass 1.7. Everything is generated from smooth noise and a
 * hash, so the world never repeats visibly and needs no stored artwork.
 *
 * Per-column heights are worked out once per frame in bd_world_prepare;
 * each strip then only compares rows against them.
 */

#define TWO_PI 6.28318531f
#define NO_CLIP 1e9f

struct biome {
    uint32_t sky[5];
    uint32_t sun;
    int16_t sun_x, sun_y, sun_r;
    bool stars;
    uint32_t shadow, light;
    uint32_t cloud, far, near, hill, tree, bush, wood, grass, dirt, stone, fg, dust;
};

/* Sunset, night and day; the active layer picks one */
static const struct biome biomes[BD_BIOME_COUNT] = {
    {.sky = {0x2a0a5e, 0x6a1b9a, 0xc2185b, 0xff5e3a, 0xffb347}, .sun = 0xffe08a,
     .sun_x = 300, .sun_y = 80, .sun_r = 24, .shadow = 0x2a0a3c, .light = 0xffd9a0,
     .cloud = 0xff8fa3, .far = 0x8a55b8, .near = 0x5a3290, .hill = 0x2e4a6a, .tree = 0x1f6a55,
     .bush = 0x2a6a40, .wood = 0x7a4430, .grass = 0x4caf50, .dirt = 0x8b5a3c, .stone = 0xa08070,
     .fg = 0x1c3324, .dust = 0xffd6a5},
    {.sky = {0x050a24, 0x0b1640, 0x13285e, 0x1d3a74, 0x2a4a80}, .sun = 0xeef3ff,
     .sun_x = 110, .sun_y = 30, .sun_r = 11, .stars = true, .shadow = 0x02040f,
     .light = 0xa8c0ff, .cloud = 0x3a4a7a, .far = 0x3a4a7a, .near = 0x25355f, .hill = 0x1a2c48,
     .tree = 0x173d3a, .bush = 0x16402e, .wood = 0x4a3030, .grass = 0x2f7a52, .dirt = 0x4a3426,
     .stone = 0x6a6a7a, .fg = 0x0a1612, .dust = 0xa9b8c8},
    {.sky = {0x2f80ed, 0x56a0f2, 0x86c2f7, 0xb8dcfb, 0xe6f3ff}, .sun = 0xfff6c8,
     .sun_x = 340, .sun_y = 26, .sun_r = 12, .shadow = 0x1d3557, .light = 0xfffbe6,
     .cloud = 0xffffff, .far = 0x9ab4e0, .near = 0x7393c8, .hill = 0x5aa05a, .tree = 0x2e7d32,
     .bush = 0x3c9a44, .wood = 0x8d6e63, .grass = 0x66bb6a, .dirt = 0xa1704a, .stone = 0xb8a898,
     .fg = 0x1f4a2c, .dust = 0xfff1d0},
};

static int biome = -1;
static struct {
    uint16_t cloud[5], far[5], near[5], hill[5], tree[5], bush[5];
    uint16_t wood[5], grass[5], dirt[5], stone[5], fg[5], dust[5];
} R;

/* The sky is banded by row; the sun and its two glow rings sit on top */
static uint16_t sky_row[BD_GY], glow_in[BD_GY], glow_out[BD_GY], sun_colour;
static int16_t sun_x, sun_y, sun_r;

#define MAX_STARS 96
static int16_t star_x[MAX_STARS], star_y[MAX_STARS];
static uint16_t star_colour[MAX_STARS];
static int star_count;

void bd_world_set_biome(int b) {
    if (b == biome) {
        return;
    }
    biome = b;
    const struct biome *B = &biomes[b];

    bd_ramp(R.cloud, B->cloud, B->shadow, B->light);
    /* Far mountains sit in the haze: a third of the way to the horizon colour */
    bd_ramp(R.far, bd_mix(B->far, B->sky[4], 0.3f), B->shadow, B->light);
    bd_ramp(R.near, B->near, B->shadow, B->light);
    bd_ramp(R.hill, B->hill, B->shadow, B->light);
    bd_ramp(R.tree, B->tree, B->shadow, B->light);
    bd_ramp(R.bush, B->bush, B->shadow, B->light);
    bd_ramp(R.wood, B->wood, B->shadow, B->light);
    bd_ramp(R.grass, B->grass, B->shadow, B->light);
    bd_ramp(R.dirt, B->dirt, B->shadow, B->light);
    bd_ramp(R.stone, B->stone, B->shadow, B->light);
    bd_ramp(R.fg, B->fg, B->shadow, B->light);
    bd_ramp(R.dust, B->dust, B->shadow, 0xffffff);

    for (int y = 0; y < BD_GY; y++) {
        /* 20 flat bands from top to horizon, blending the five sky stops */
        const float t = (floorf((float)y / BD_GY * 20.0f) + 0.5f) / 20.0f * 4.0f;
        int i0 = (int)t;
        if (i0 > 3) {
            i0 = 3;
        }
        const uint32_t c0 = bd_mix(B->sky[i0], B->sky[i0 + 1], t - i0);
        sky_row[y] = bd_rgb(c0);
        glow_in[y] = bd_rgb(bd_mix(c0, B->sun, 0.35f));
        glow_out[y] = bd_rgb(bd_mix(c0, B->sun, 0.16f));
    }
    sun_colour = bd_rgb(B->sun);
    sun_x = B->sun_x;
    sun_y = B->sun_y;
    sun_r = B->sun_r;

    star_count = 0;
    if (B->stars) {
        for (int y = 0; y < 80; y++) {
            for (int x = 0; x < BD_W && star_count < MAX_STARS; x++) {
                if (bd_hash((uint32_t)(x * 977 + y * 131)) < 0.006f) {
                    star_x[star_count] = x;
                    star_y[star_count] = y;
                    star_colour[star_count] =
                        bd_rgb(bd_hash((uint32_t)(x + y * 7)) < 0.5f ? 0xffffff : 0x96b4ff);
                    star_count++;
                }
            }
        }
    }
}

const uint16_t *bd_world_dust(void) { return R.dust; }

static float far_h(float wx) {
    const float a = wx * (TWO_PI / 700.0f);
    return 30 + 12 * sinf(a * 2) + 7 * sinf(a * 5 + 1.3f) + 4 * sinf(a * 11 + 0.4f);
}

static float near_h(float wx) {
    const float a = wx * (TWO_PI / 520.0f);
    return 20 + 9 * sinf(a * 3 + 2) + 5 * sinf(a * 7 + 0.5f);
}

static float hill_h(float wx) {
    const float a = wx * (TWO_PI / 400.0f);
    return 15 + 5 * sinf(a * 3) + 3 * sinf(a * 8 + 1);
}

/* Scroll position, kept in double so it stays exact over hours of typing */
static double scroll;
static uint32_t tick;

static int16_t far_top[BD_W], near_top[BD_W], hill_top[BD_W], bush_top[BD_W];
static uint8_t far_level[BD_W], far_ridge[BD_W], near_level[BD_W];
static bool near_snow[BD_W];
static uint8_t blade_h[BD_W];
static uint32_t ground_wx0;

void bd_world_prepare(double s, uint32_t t) {
    scroll = s;
    tick = t;
    /* Static: 1.7 KB is a lot for the display queue's stack */
    static float h[BD_W + 2];

    /* Each layer's offset is folded into its period, so sinf sees small values */
    const float fo = (float)fmod(s * 0.08, 700.0);
    for (int i = 0; i < BD_W + 2; i++) {
        h[i] = far_h(i - 1 + fo);
    }
    for (int x = 0; x < BD_W; x++) {
        const float slope = h[x + 2] - h[x];
        far_top[x] = (int16_t)floorf(BD_GY - 12 - h[x + 1]);
        far_ridge[x] = slope > 0 ? 4 : 3;
        far_level[x] = slope > 0.4f ? 3 : slope < -0.4f ? 1 : 2;
    }

    const float no = (float)fmod(s * 0.16, 520.0);
    for (int i = 0; i < BD_W + 2; i++) {
        h[i] = near_h(i - 1 + no);
    }
    for (int x = 0; x < BD_W; x++) {
        const float slope = h[x + 2] - h[x];
        near_top[x] = (int16_t)floorf(BD_GY - 6 - h[x + 1]);
        near_level[x] = slope > 0.35f ? 3 : slope < -0.35f ? 1 : 2;
        near_snow[x] = h[x + 1] > 26;
    }

    const float ho = (float)fmod(s * 0.32, 400.0);
    const float bo = (float)fmod(s * 0.6, 260.0);
    for (int x = 0; x < BD_W; x++) {
        hill_top[x] = (int16_t)floorf(BD_GY - hill_h(x + ho));
        const float a = (x + bo) * (TWO_PI / 260.0f);
        bush_top[x] = (int16_t)(BD_GY - (int)(4 + 3 * fabsf(sinf(a * 4)) + 2 * fabsf(sinf(a * 9))));
    }

    /* Ground and grass move one-to-one with the hero, so a whole pixel step */
    const int64_t base = (int64_t)floor(s);
    ground_wx0 = (uint32_t)base;
    for (int x = 0; x < BD_W; x++) {
        const int64_t wx = base + x;
        blade_h[x] = (uint8_t)(bd_hash((uint32_t)wx) * 4) + ((wx % 3) == 0 ? 2 : 0);
    }
}

static void draw_sky(int y, uint16_t *row) {
    const uint16_t c = sky_row[y];
    for (int x = 0; x < BD_W; x++) {
        row[x] = c;
    }
    const float dy = y + 0.5f - sun_y, outer = sun_r + 11.0f;
    if (fabsf(dy) > outer) {
        return;
    }
    const float span = sqrtf(outer * outer - dy * dy);
    const int xa = MAX(0, (int)floorf(sun_x - span)), xb = MIN(BD_W - 1, (int)(sun_x + span));
    for (int x = xa; x <= xb; x++) {
        const float dx = x + 0.5f - sun_x, d = sqrtf(dx * dx + dy * dy);
        if (d <= sun_r) {
            row[x] = sun_colour;
        } else if (d <= sun_r + 5) {
            row[x] = glow_in[y];
        } else if (d <= outer) {
            row[x] = glow_out[y];
        }
    }
}

static const int16_t clouds[][3] = {{40, 22, 9}, {190, 13, 7}, {330, 32, 11}, {520, 18, 8}, {650, 28, 10}};
static const float puffs[][3] = {
    {-1.3f, 0, 0.65f}, {-0.5f, -0.55f, 0.85f}, {0.45f, -0.4f, 0.8f}, {1.25f, 0.05f, 0.6f}, {0, 0.15f, 0.75f}};

static void draw_trees(void) {
    const double ho = scroll * 0.32;
    const int32_t k0 = (int32_t)floor(ho / 46.0) - 1;
    for (int32_t k = k0; k < k0 + 12; k++) {
        const float hk = bd_hash((uint32_t)k);
        const float tx = (float)(k * 46.0 + hk * 20.0 - ho);
        if (tx < -24 || tx > BD_W + 24) {
            continue;
        }
        const float by = BD_GY - hill_h((float)fmod(k * 46.0 + hk * 20.0, 400.0)) + 2;
        const float th = 10 + bd_hash((uint32_t)(k + 5)) * 8;
        const float cr = 7 + bd_hash((uint32_t)(k + 9)) * 3;
        const int x = (int)tx;
        for (int y = MAX(bd_y0, (int)ceilf(by - th)); y < MIN(bd_y1, (int)ceilf(by)); y++) {
            bd_put(x - 1, y, R.wood[3]);
            bd_put(x, y, R.wood[2]);
            bd_put(x + 1, y, R.wood[1]);
        }
        bd_disc(tx - 4, by - th + 3, cr * 0.7f, R.tree, NO_CLIP);
        bd_disc(tx + 4, by - th + 2, cr * 0.75f, R.tree, NO_CLIP);
        bd_disc(tx, by - th - 2, cr, R.tree, NO_CLIP);
    }
}

static void draw_fence(void) {
    const double no = scroll * 0.6;
    const int32_t p0 = (int32_t)floor(no / 42.0) - 1;
    for (int32_t k = p0; k < p0 + 13; k++) {
        const int x = (int)lround(k * 42.0 - no);
        for (int i = 0; i < 42; i++) {
            bd_put(x + i, BD_GY - 13, R.wood[3]);
            bd_put(x + i, BD_GY - 12, R.wood[1]);
            bd_put(x + i, BD_GY - 7, R.wood[3]);
            bd_put(x + i, BD_GY - 6, R.wood[1]);
        }
        for (int y = MAX(bd_y0, BD_GY - 17); y < MIN(bd_y1, BD_GY); y++) {
            bd_put(x - 1, y, R.wood[y < BD_GY - 15 ? 4 : 3]);
            bd_put(x, y, R.wood[2]);
            bd_put(x + 1, y, R.wood[1]);
        }
    }
}

void bd_world_draw_back(void) {
    const int sky_end = MIN(bd_y1, BD_GY);

    for (int y = bd_y0; y < sky_end; y++) {
        draw_sky(y, bd_row(y));
    }
    for (int i = 0; i < star_count; i++) {
        bd_put(star_x[i], star_y[i], star_colour[i]);
    }

    for (size_t c = 0; c < ARRAY_SIZE(clouds); c++) {
        const float s = clouds[c][2];
        double cx = fmod(clouds[c][0] - scroll * 0.06 - tick * 0.08, 760.0);
        if (cx < 0) {
            cx += 760.0;
        }
        cx -= 60.0;
        const float cy = clouds[c][1];
        for (size_t p = 0; p < ARRAY_SIZE(puffs); p++) {
            bd_disc((float)cx + puffs[p][0] * s * 2, cy + puffs[p][1] * s * 2, puffs[p][2] * s * 2,
                    R.cloud, cy + s * 0.7f);
        }
    }

    /* Far range, near range with snowy peaks, then the hills, front to back by overwrite */
    for (int y = bd_y0; y < sky_end; y++) {
        uint16_t *row = bd_row(y);
        for (int x = 0; x < BD_W; x++) {
            if (y >= far_top[x]) {
                row[x] = R.far[y == far_top[x] ? far_ridge[x] : far_level[x]];
            }
            if (y >= near_top[x]) {
                int l = near_level[x];
                if (near_snow[x] && y < near_top[x] + 3) {
                    l = 4;
                } else if (y == near_top[x]) {
                    l = MIN(4, l + 1);
                }
                row[x] = R.near[l];
            }
            if (y >= hill_top[x]) {
                const int d = y - hill_top[x];
                row[x] = R.hill[d < 2 ? 3 : d < 5 ? 2 : 1];
            }
        }
    }

    draw_trees();

    for (int y = bd_y0; y < sky_end; y++) {
        uint16_t *row = bd_row(y);
        for (int x = 0; x < BD_W; x++) {
            if (y >= bush_top[x]) {
                const int d = y - bush_top[x];
                row[x] = R.bush[d == 0 ? 4 : d == 1 ? 3 : y > BD_GY - 2 ? 1 : 2];
            }
        }
    }

    draw_fence();

    for (int y = MAX(bd_y0, BD_GY); y < bd_y1; y++) {
        uint16_t *row = bd_row(y);
        const int r = y - BD_GY;
        for (int x = 0; x < BD_W; x++) {
            const uint32_t wx = ground_wx0 + (uint32_t)x;
            if (r < 3) {
                row[x] = R.grass[4 - r];
            } else if (r < 5) {
                row[x] = bd_hash(wx * 31 + y) < 0.5f ? R.grass[1] : R.dirt[2];
            } else {
                const float n = bd_hash(wx * 31 + y * 7);
                row[x] = R.dirt[n < 0.08f ? 1 : n > 0.94f ? 3 : 2];
            }
        }
    }

    /* Grass blades standing up out of the ground line */
    for (int y = MAX(bd_y0, BD_GY - 6); y < MIN(bd_y1, BD_GY); y++) {
        const int i = BD_GY - y;
        uint16_t *row = bd_row(y);
        for (int x = 0; x < BD_W; x++) {
            if (blade_h[x] >= i) {
                row[x] = R.grass[i == blade_h[x] ? 4 : 3];
            }
        }
    }

    const int32_t s0 = (int32_t)floor(scroll / 37.0) - 1;
    for (int32_t k = s0; k < s0 + 14; k++) {
        const float sx = (float)(k * 37.0 + bd_hash((uint32_t)k) * 20.0 - scroll);
        bd_ellipse(sx, BD_GY + 10 + bd_hash((uint32_t)(k + 3)) * 8,
                   3 + bd_hash((uint32_t)(k + 1)) * 3, 2 + bd_hash((uint32_t)(k + 2)) * 1.5f, R.stone);
    }
}

void bd_world_draw_front(void) {
    /* Tall grass clumps sweeping past in front of the hero */
    const double fo = scroll * 1.7;
    const int32_t f0 = (int32_t)floor(fo / 170.0) - 1;
    for (int32_t k = f0; k < f0 + 5; k++) {
        const float bx = (float)(k * 170.0 + bd_hash((uint32_t)k) * 80.0 - fo);
        if (bx < -40 || bx > BD_W + 20) {
            continue;
        }
        for (int b = 0; b < 7; b++) {
            const float h = 18 + bd_hash((uint32_t)(k * 7 + b)) * 16;
            const float lean = (bd_hash((uint32_t)(k * 13 + b)) - 0.5f) * 1.6f;
            const float x0 = bx + b * 3;
            for (int i = 0; i < h; i++) {
                const int y = BD_H - 1 - i;
                if (y < bd_y0 || y >= bd_y1) {
                    continue;
                }
                const float t = i / h;
                const int x = (int)(x0 + lean * t * t * h * 0.5f);
                bd_put(x, y, R.fg[3]);
                bd_put(x + 1, y, R.fg[1]);
            }
        }
    }
}
