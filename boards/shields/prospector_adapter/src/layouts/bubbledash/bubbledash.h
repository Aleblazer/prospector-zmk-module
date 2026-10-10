#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * BubbleDash draws the whole 428x142 screen itself, one horizontal strip at
 * a time, and writes each strip straight to the display. Every pixel moves
 * every frame, so a full frame buffer (121 KB) would not fit, and LVGL would
 * redraw the same pixels the hard way; LVGL only shows a black screen once.
 */
#define BD_W 428
#define BD_H 142
#define BD_GY 118 /* the ground line */
#define BD_HX 132 /* where the hero stands on screen */
#define BD_STRIP_H 16
#define BD_BIOME_COUNT 3

/* The strip being drawn: screen rows [bd_y0, bd_y1), BD_W pixels per row */
extern uint16_t *bd_strip;
extern int bd_y0, bd_y1;

static inline uint16_t *bd_row(int y) { return bd_strip + (y - bd_y0) * BD_W; }

static inline void bd_put(int x, int y, uint16_t c) {
    if ((unsigned)x < BD_W && y >= bd_y0 && y < bd_y1) {
        bd_row(y)[x] = c;
    }
}

/*
 * Shade level 1-4 for a surface normal, lit from the top left and front;
 * level 0 is reserved for outlines. bias darkens parts further back.
 */
static inline int bd_level(float nx, float ny, float nz, int bias) {
    const float d = nx * -0.55f + ny * -0.65f + nz * 0.52f;
    int l = d < -0.15f ? 1 : d < 0.25f ? 2 : d < 0.62f ? 3 : 4;
    l -= bias;
    return l < 1 ? 1 : l;
}

/* Colours: 0xRRGGBB in; RGB565 out with its bytes swapped, as the panel takes it */
uint16_t bd_rgb(uint32_t hex);
uint32_t bd_mix(uint32_t a, uint32_t b, float t);
/* Five tones: two shadows, the base, two highlights */
void bd_ramp(uint16_t out[5], uint32_t base, uint32_t shadow, uint32_t light);
uint16_t bd_darken(uint16_t c);
uint16_t bd_blend(uint16_t bg, uint32_t fg, uint8_t alpha);
float bd_hash(uint32_t n);

/* Shaded disc and ellipse into the current strip; rows below clip_y are skipped */
void bd_disc(float cx, float cy, float r, const uint16_t ramp[5], float clip_y);
void bd_ellipse(float cx, float cy, float rx, float ry, const uint16_t ramp[5]);

/* The world: sky, clouds, mountains, hills, trees, fence, ground, foreground */
void bd_world_set_biome(int biome);
void bd_world_prepare(double scroll, uint32_t tick);
void bd_world_draw_back(void);
void bd_world_draw_front(void);
const uint16_t *bd_world_dust(void);

/* The hero, drawn into an 80x80 sprite whose feet rest on row 72 */
#define BD_HERO_SIZE 80
#define BD_HERO_BX 40
#define BD_HERO_FY 69
#define BD_HERO_GROUND 72
#define BD_HERO_BR 19.0f
#define BD_HERO_BRY 18.0f

struct bd_pose {
    /* Body radii, hop height, lean and somersault angle (radians) */
    float rx, ry, lift, tilt, rot;
    /* Body centre and feet, in sprite pixels */
    float cx, cy;
    float ffx, ffy, bfx, bfy;
    /* Hands relative to the body centre, in units of the resting body */
    float nearx, neary, farx, fary;
    bool blink, mouth_open;
    int look;
};

void bd_hero_init(void);
void bd_hero_leaf_target(const struct bd_pose *p, float *x, float *y);
void bd_hero_build(const struct bd_pose *p, float leaf_x, float leaf_y);
void bd_hero_draw(int sox, int soy, bool mirror);
/* The hero's own colours, for sparks and speed lines: 1 body, 4 leaf, 6 blush */
uint16_t bd_hero_colour(int mat, int level);

/* Starts the frame loop; called once when the status screen is created */
void bd_start(void);
