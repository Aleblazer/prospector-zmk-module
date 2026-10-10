#include "bubbledash.h"

#include <math.h>
#include <string.h>
#include <zephyr/sys/util.h>

/*
 * The hero: a round mint blob with a leaf sprout, two hand nubs and two
 * feet. Each frame he is drawn into an 80x80 sprite one part at a time, from
 * the far hand to the near hand. Every pixel keeps its material, its shade
 * level and the part that drew it. Where a part lands on an earlier one its
 * edge pixels drop to the darkest shade, which inks the overlaps, and a last
 * pass rings the whole figure with each material's outline colour.
 */

#define S BD_HERO_SIZE

enum { MAT_NONE, MAT_BODY, MAT_FEET, MAT_LEAF = 4, MAT_EYE, MAT_BLUSH, MAT_COUNT };

#define PART_FACE 99
#define OUTLINE 0x80

static const uint32_t palette[MAT_COUNT][5] = {
    [MAT_BODY] = {0x1d4a3c, 0x3a8a72, 0x66c4a3, 0x9fe8cb, 0xe0fff2},
    [MAT_FEET] = {0x0d1126, 0x1c2548, 0x2f3c72, 0x4b5f9f, 0x8798cf},
    [MAT_LEAF] = {0x173d12, 0x2f6f1f, 0x4fa52c, 0x86d24a, 0xc9f38c},
    [MAT_EYE] = {0x14122a, 0x14122a, 0x2b3a8a, 0x6f86e0, 0xffffff},
    [MAT_BLUSH] = {0x7a2a4a, 0xc24a78, 0xff7aa8, 0xff9fc0, 0xffd0e0},
};

/* Sprite colours by (material << 3 | level); an outline pixel has level 0 */
static uint16_t colours[MAT_COUNT << 3];

/* (material << 3) | level, OUTLINE for the ring pass; 0 is empty */
static uint8_t pix[S * S];
static uint8_t part_of[S * S];

static uint8_t cur_part;
static int bias;
static int bx0, by0, bx1, by1; /* the current part's bounding box */

/* Rotation of the whole figure about the body centre, for somersaults */
static float pvx, pvy, rc = 1, rs;

void bd_hero_init(void) {
    for (int m = 0; m < MAT_COUNT; m++) {
        for (int l = 0; l < 5; l++) {
            colours[(m << 3) | l] = palette[m][l] ? bd_rgb(palette[m][l]) : 0;
        }
    }
}

uint16_t bd_hero_colour(int mat, int level) { return colours[(mat << 3) | level]; }

static void turn(float x, float y, float *ox, float *oy) {
    const float dx = x - pvx, dy = y - pvy;
    *ox = pvx + dx * rc - dy * rs;
    *oy = pvy + dx * rs + dy * rc;
}

/* A point on the body, in resting-body pixels from its centre, leaned and turned */
static void body_pt(const struct bd_pose *p, float dx, float dy, float *ox, float *oy) {
    const float ct = cosf(p->tilt), st = sinf(p->tilt);
    turn(p->cx + dx * ct - dy * st, p->cy + dx * st + dy * ct, ox, oy);
}

static void plot(int x, int y, int mat, int level) {
    if ((unsigned)x >= S || (unsigned)y >= S) {
        return;
    }
    const int i = y * S + x;
    pix[i] = (uint8_t)((mat << 3) | level);
    part_of[i] = cur_part;
    bx0 = MIN(bx0, x);
    by0 = MIN(by0, y);
    bx1 = MAX(bx1, x);
    by1 = MAX(by1, y);
}

static void disc(float cx, float cy, float r, int mat) {
    const float r2 = r * r;
    for (int y = (int)floorf(cy - r); y <= cy + r; y++) {
        for (int x = (int)floorf(cx - r); x <= cx + r; x++) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, q = dx * dx + dy * dy;
            if (q <= r2) {
                plot(x, y, mat, bd_level(dx / r, dy / r, sqrtf(1 - q / r2), bias));
            }
        }
    }
}

/* A round-ended stroke from a to b */
static void capsule(float ax, float ay, float bx, float by, float r, int mat) {
    const float ex = bx - ax, ey = by - ay, r2 = r * r;
    float l2 = ex * ex + ey * ey;
    if (l2 == 0) {
        l2 = 1;
    }
    for (int y = (int)floorf(MIN(ay, by) - r); y <= MAX(ay, by) + r; y++) {
        for (int x = (int)floorf(MIN(ax, bx) - r); x <= MAX(ax, bx) + r; x++) {
            const float qx = x + 0.5f, qy = y + 0.5f;
            float t = ((qx - ax) * ex + (qy - ay) * ey) / l2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float dx = qx - (ax + ex * t), dy = qy - (ay + ey * t), q = dx * dx + dy * dy;
            if (q <= r2) {
                plot(x, y, mat, bd_level(dx / r, dy / r, sqrtf(1 - q / r2), bias));
            }
        }
    }
}

/* An ellipse turned by ang; its shading normal turns with it */
static void ellipse(float cx, float cy, float rx, float ry, float ang, int mat) {
    const float co = cosf(ang), si = sinf(ang), rr = MAX(rx, ry);
    for (int y = (int)floorf(cy - rr); y <= cy + rr; y++) {
        for (int x = (int)floorf(cx - rr); x <= cx + rr; x++) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float u = (dx * co + dy * si) / rx, v = (-dx * si + dy * co) / ry;
            const float q = u * u + v * v;
            if (q <= 1) {
                plot(x, y, mat, bd_level(u * co - v * si, u * si + v * co, sqrtf(1 - q), bias));
            }
        }
    }
}

static void part_begin(uint8_t id, int b) {
    cur_part = id;
    bias = b;
    bx0 = by0 = S;
    bx1 = by1 = -1;
}

/* Ink this part's pixels that touch an earlier part (but not the face) */
static void part_end(void) {
    for (int y = by0; y <= by1; y++) {
        for (int x = bx0; x <= bx1; x++) {
            const int i = y * S + x;
            if (part_of[i] != cur_part || !pix[i]) {
                continue;
            }
            const int nb[4] = {x > 0 ? i - 1 : -1, x < S - 1 ? i + 1 : -1, y > 0 ? i - S : -1,
                               y < S - 1 ? i + S : -1};
            for (int k = 0; k < 4; k++) {
                const int j = nb[k];
                if (j >= 0 && pix[j] && part_of[j] != cur_part && part_of[j] != PART_FACE) {
                    if ((pix[i] & 7) > 1) {
                        pix[i] = (pix[i] & ~7) | 1;
                    }
                    break;
                }
            }
        }
    }
    bias = 0;
    cur_part = 0;
}

/* A face pixel: only drawn over the body, never past its edge */
static void face_px(const struct bd_pose *p, float sx, float sy, float dx, float dy, int mat,
                    int level) {
    float fx, fy;
    body_pt(p, (dx + 0.5f) * sx, (dy + 0.5f) * sy, &fx, &fy);
    const int x = (int)floorf(fx), y = (int)floorf(fy);
    if ((unsigned)x >= S || (unsigned)y >= S) {
        return;
    }
    const int i = y * S + x;
    if (!pix[i]) {
        return;
    }
    pix[i] = (uint8_t)((mat << 3) | level);
    part_of[i] = PART_FACE;
}

static void eye(const struct bd_pose *p, float sx, float sy, int ox, int oy, int w, int h) {
    const int lk = p->look;
    if (p->blink) {
        for (int x = 0; x < w; x++) {
            face_px(p, sx, sy, ox + x, oy + h - 3, MAT_EYE, 1);
        }
        return;
    }
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            const float u = (x + 0.5f - w / 2.0f) / (w / 2.0f), v = (y + 0.5f - h / 2.0f) / (h / 2.0f);
            if (u * u + v * v <= 1.15f) {
                face_px(p, sx, sy, ox + x + lk, oy + y, MAT_EYE, y >= h - 2 ? 2 : 1);
            }
        }
    }
    /* A three-pixel catchlight, and a glint low on the wider eye */
    face_px(p, sx, sy, ox + lk, oy + 1, MAT_EYE, 4);
    face_px(p, sx, sy, ox + 1 + lk, oy + 1, MAT_EYE, 4);
    face_px(p, sx, sy, ox + lk, oy + 2, MAT_EYE, 4);
    if (w > 3) {
        face_px(p, sx, sy, ox + w - 2 + lk, oy + h - 2, MAT_EYE, 3);
    }
}

void bd_hero_leaf_target(const struct bd_pose *p, float *x, float *y) {
    rc = cosf(p->rot);
    rs = sinf(p->rot);
    pvx = p->cx;
    pvy = p->cy;
    const float a = p->tilt + p->rot, ca = cosf(a), sa = sinf(a);
    float bx, by;
    body_pt(p, -3, -p->ry + 1.5f, &bx, &by);
    *x = bx + (-5 * ca + 9 * sa);
    *y = by + (-5 * sa - 9 * ca);
}

void bd_hero_build(const struct bd_pose *p, float leaf_x, float leaf_y) {
    memset(pix, 0, sizeof(pix));
    memset(part_of, 0, sizeof(part_of));
    rc = cosf(p->rot);
    rs = sinf(p->rot);
    pvx = p->cx;
    pvy = p->cy;
    const float sx = p->rx / BD_HERO_BR, sy = p->ry / BD_HERO_BRY, ang = p->tilt + p->rot;
    float x, y;

    /* Far hand and back foot first, a shade darker for depth */
    part_begin(1, 1);
    body_pt(p, p->farx * sx, p->fary * sy, &x, &y);
    disc(x, y, 4, MAT_BODY);
    part_end();

    part_begin(2, 1);
    turn(p->bfx, p->bfy, &x, &y);
    ellipse(x, y, 6.5f, 3.8f, p->rot, MAT_FEET);
    part_end();

    part_begin(4, 0);
    ellipse(p->cx, p->cy, p->rx, p->ry, ang, MAT_BODY);
    part_end();

    /* The sprout: a stem to the trailing leaf position and two leaves */
    part_begin(6, 0);
    body_pt(p, -3 * sx, -p->ry + 1.5f, &x, &y);
    const float la = atan2f(leaf_y - y, leaf_x - x);
    capsule(x, y, leaf_x, leaf_y, 1.3f, MAT_LEAF);
    ellipse(leaf_x, leaf_y, 5.6f, 2.8f, la + 0.9f, MAT_LEAF);
    ellipse(leaf_x + cosf(la + 2.4f) * 3, leaf_y + sinf(la + 2.4f) * 3, 4, 2.2f, la - 0.9f,
            MAT_LEAF);
    part_end();

    eye(p, sx, sy, 4, -8, 4, 7);
    eye(p, sx, sy, 11, -8, 3, 6);
    for (int i = 1; i <= 3; i++) {
        face_px(p, sx, sy, i, 0, MAT_BLUSH, i == 3 ? 4 : 3);
    }
    face_px(p, sx, sy, 13, 0, MAT_BLUSH, 3);
    face_px(p, sx, sy, 14, 0, MAT_BLUSH, 3);
    if (p->mouth_open) {
        for (int mx = 8; mx <= 10; mx++) {
            for (int my = 0; my <= 2; my++) {
                face_px(p, sx, sy, mx, my, MAT_EYE, 1);
            }
        }
        face_px(p, sx, sy, 9, 2, MAT_BLUSH, 2);
    } else {
        face_px(p, sx, sy, 8, 0, MAT_EYE, 1);
        face_px(p, sx, sy, 9, 1, MAT_EYE, 1);
        face_px(p, sx, sy, 10, 0, MAT_EYE, 1);
    }

    part_begin(7, 0);
    turn(p->ffx, p->ffy, &x, &y);
    ellipse(x, y, 6.5f, 3.8f, p->rot, MAT_FEET);
    part_end();

    part_begin(8, 0);
    body_pt(p, p->nearx * sx, p->neary * sy, &x, &y);
    disc(x, y, 4.6f, MAT_BODY);
    part_end();

    /* Ring every empty pixel touching the figure in its neighbour's outline shade */
    for (int yy = 0; yy < S; yy++) {
        for (int xx = 0; xx < S; xx++) {
            const int i = yy * S + xx;
            if (pix[i]) {
                continue;
            }
            uint8_t m = 0;
            if (xx > 0 && pix[i - 1] && !(pix[i - 1] & OUTLINE)) {
                m = pix[i - 1];
            } else if (xx < S - 1 && pix[i + 1] && !(pix[i + 1] & OUTLINE)) {
                m = pix[i + 1];
            } else if (yy > 0 && pix[i - S] && !(pix[i - S] & OUTLINE)) {
                m = pix[i - S];
            } else if (yy < S - 1 && pix[i + S] && !(pix[i + S] & OUTLINE)) {
                m = pix[i + S];
            }
            if (m) {
                pix[i] = OUTLINE | (m & ~7);
            }
        }
    }
}

void bd_hero_draw(int sox, int soy, bool mirror) {
    const int ya = MAX(bd_y0, soy), yb = MIN(bd_y1, soy + S);
    for (int y = ya; y < yb; y++) {
        const uint8_t *src = &pix[(y - soy) * S];
        uint16_t *row = bd_row(y);
        for (int x = 0; x < S; x++) {
            const uint8_t v = src[mirror ? S - 1 - x : x];
            const int dx = sox + x;
            if (v && (unsigned)dx < BD_W) {
                row[dx] = colours[v & 0x3f];
            }
        }
    }
}
