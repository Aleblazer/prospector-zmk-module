#include "bubbledash.h"

#include <math.h>

uint16_t *bd_strip;
int bd_y0, bd_y1;

uint16_t bd_rgb(uint32_t hex) {
    const uint16_t c = ((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F);
    return (uint16_t)((c >> 8) | (c << 8));
}

uint32_t bd_mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        const float ca = (float)((a >> s) & 0xFF);
        const float cb = (float)((b >> s) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

void bd_ramp(uint16_t out[5], uint32_t base, uint32_t shadow, uint32_t light) {
    out[0] = bd_rgb(bd_mix(base, shadow, 0.72f));
    out[1] = bd_rgb(bd_mix(base, shadow, 0.42f));
    out[2] = bd_rgb(base);
    out[3] = bd_rgb(bd_mix(base, light, 0.38f));
    out[4] = bd_rgb(bd_mix(base, light, 0.72f));
}

uint16_t bd_darken(uint16_t c) {
    uint16_t v = (uint16_t)((c >> 8) | (c << 8));
    /* Halve every channel: drop each one's low bit, then shift */
    v = (v & 0xF7DE) >> 1;
    return (uint16_t)((v >> 8) | (v << 8));
}

uint16_t bd_blend(uint16_t bg, uint32_t fg, uint8_t alpha) {
    const uint16_t v = (uint16_t)((bg >> 8) | (bg << 8));
    const int br = ((v >> 11) & 0x1F) << 3, bgc = ((v >> 5) & 0x3F) << 2, bb = (v & 0x1F) << 3;
    const int fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    const int r = br + ((fr - br) * alpha) / 255;
    const int g = bgc + ((fgc - bgc) * alpha) / 255;
    const int b = bb + ((fb - bb) * alpha) / 255;
    return bd_rgb(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

float bd_hash(uint32_t n) {
    n *= 2654435761u;
    n ^= n >> 15;
    n *= 2246822519u;
    n ^= n >> 13;
    return (float)n * (1.0f / 4294967296.0f);
}

void bd_disc(float cx, float cy, float r, const uint16_t ramp[5], float clip_y) {
    int ya = (int)floorf(cy - r);
    int yb = (int)floorf(cy + r);
    if (yb > (int)floorf(clip_y)) {
        yb = (int)floorf(clip_y);
    }
    if (ya < bd_y0) {
        ya = bd_y0;
    }
    if (yb > bd_y1 - 1) {
        yb = bd_y1 - 1;
    }
    const float r2 = r * r, inv = 1.0f / r;
    for (int y = ya; y <= yb; y++) {
        const float dy = y + 0.5f - cy;
        const float span2 = r2 - dy * dy;
        if (span2 < 0.0f) {
            continue;
        }
        const float span = sqrtf(span2);
        int xa = (int)floorf(cx - span), xb = (int)floorf(cx + span);
        if (xa < 0) {
            xa = 0;
        }
        if (xb > BD_W - 1) {
            xb = BD_W - 1;
        }
        uint16_t *row = bd_row(y);
        for (int x = xa; x <= xb; x++) {
            const float dx = x + 0.5f - cx, q = dx * dx + dy * dy;
            if (q <= r2) {
                row[x] = ramp[bd_level(dx * inv, dy * inv, sqrtf(1.0f - q / r2), 0)];
            }
        }
    }
}

void bd_ellipse(float cx, float cy, float rx, float ry, const uint16_t ramp[5]) {
    int ya = (int)floorf(cy - ry), yb = (int)floorf(cy + ry);
    if (ya < bd_y0) {
        ya = bd_y0;
    }
    if (yb > bd_y1 - 1) {
        yb = bd_y1 - 1;
    }
    int xa = (int)floorf(cx - rx), xb = (int)floorf(cx + rx);
    if (xa < 0) {
        xa = 0;
    }
    if (xb > BD_W - 1) {
        xb = BD_W - 1;
    }
    for (int y = ya; y <= yb; y++) {
        const float v = (y + 0.5f - cy) / ry;
        uint16_t *row = bd_row(y);
        for (int x = xa; x <= xb; x++) {
            const float u = (x + 0.5f - cx) / rx, q = u * u + v * v;
            if (q <= 1.0f) {
                row[x] = ramp[bd_level(u, v, sqrtf(1.0f - q), 0)];
            }
        }
    }
}
