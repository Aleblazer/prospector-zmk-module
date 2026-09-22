#include "layer_indicator.h"

#include <math.h>
#include <ctype.h>
#include <zmk/display.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#include <fonts.h>
#include "display_colors.h"

#define WHEEL_SIZE 48
#define WHEEL_CENTER (WHEEL_SIZE / 2)
#define WHEEL_INNER_RADIUS 12
#define WHEEL_OUTER_RADIUS 20

/* float pi, so the tick maths never promotes to double */
#define M_PI_F 3.14159265f

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct layer_indicator_state {
    uint8_t index;
};

/* Half the tick's stroke width, so ticks are 4 px wide with round ends */
#define WHEEL_TICK_HALF_WIDTH 2.0f

/*
 * The wheel is shaded pixel by pixel from each pixel's distance to the
 * nearest tick, treating a tick as a capsule: a segment from the inner to
 * the outer radius, thickened by the half width. That gives straight,
 * evenly rounded, antialiased ticks at any angle.
 *
 * Two earlier approaches bent them. Turning a bitmap with lv_image rotation
 * resampled the 4 px strokes into blobs. lv_draw_line widens slanted lines
 * to an odd width while its round caps stay on the even-width grid, so the
 * caps sat a pixel off the body and steep ticks looked hooked.
 */
static void wheel_draw(lv_obj_t *canvas, int32_t angle) {
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(canvas);
    const lv_color_t color = lv_color_hex(DISPLAY_COLOR_LAYER_WHEEL);
    const float centre = WHEEL_SIZE / 2.0f;

    /* Unit direction of each tick; angle is in tenths of a degree, tick 0 up at 0 */
    float ux[ZMK_KEYMAP_LAYERS_LEN];
    float uy[ZMK_KEYMAP_LAYERS_LEN];
    for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        float a = ((float)i * 360.0f / ZMK_KEYMAP_LAYERS_LEN + angle / 10.0f - 90.0f) * (M_PI_F / 180.0f);
        ux[i] = cosf(a);
        uy[i] = sinf(a);
    }

    for (int y = 0; y < WHEEL_SIZE; y++) {
        lv_color32_t *row = (lv_color32_t *)lv_draw_buf_goto_xy(buf, 0, y);
        for (int x = 0; x < WHEEL_SIZE; x++) {
            /* Pixel centre, relative to the wheel centre */
            const float px = x + 0.5f - centre;
            const float py = y + 0.5f - centre;

            float nearest = WHEEL_SIZE;
            for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
                /* Closest point on the tick: project onto it and clamp to its ends */
                float t = px * ux[i] + py * uy[i];
                t = t < WHEEL_INNER_RADIUS ? WHEEL_INNER_RADIUS
                    : t > WHEEL_OUTER_RADIUS ? WHEEL_OUTER_RADIUS : t;
                const float dx = px - t * ux[i];
                const float dy = py - t * uy[i];
                const float d = sqrtf(dx * dx + dy * dy);
                if (d < nearest) {
                    nearest = d;
                }
            }

            /* Full inside the stroke, fading over the pixel that straddles its edge */
            float coverage = WHEEL_TICK_HALF_WIDTH + 0.5f - nearest;
            coverage = coverage < 0.0f ? 0.0f : coverage > 1.0f ? 1.0f : coverage;

            row[x].blue = color.blue;
            row[x].green = color.green;
            row[x].red = color.red;
            row[x].alpha = (uint8_t)(coverage * 255.0f + 0.5f);
        }
    }

    lv_image_cache_drop(lv_canvas_get_image(canvas));
    lv_obj_invalidate(canvas);
}

/* Current wheel angle in tenths of a degree */
static int32_t wheel_angle;

static void wheel_angle_anim(void *canvas, int32_t v) {
    wheel_angle = v;
    wheel_draw(canvas, v);
}

static void layer_indicator_set_sel(struct zmk_widget_layer_indicator *widget, struct layer_indicator_state state) {
    // Calculate target angle: layer 0 at top (0°), going clockwise, in 0.1 degree units
    int32_t angle_per_layer = 3600 / ZMK_KEYMAP_LAYERS_LEN;
    int32_t target_angle = -(state.index * angle_per_layer);  // Negative to rotate wheel so current layer is at top

    int32_t current_angle = wheel_angle;

    // Normalize for shortest path
    int32_t diff = target_angle - current_angle;
    while (diff > 1800) { target_angle -= 3600; diff = target_angle - current_angle; }
    while (diff < -1800) { target_angle += 3600; diff = target_angle - current_angle; }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, widget->wheel);
    lv_anim_set_values(&a, current_angle, target_angle);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, wheel_angle_anim);
    lv_anim_start(&a);

    const char *layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(state.index));
    char display_name[64];

    if (layer_name && *layer_name) {
        snprintf(display_name, sizeof(display_name), "%s", layer_name);
    } else {
        snprintf(display_name, sizeof(display_name), "%d", state.index);
    }

#if IS_ENABLED(CONFIG_PROSPECTOR_LAYER_NAME_UPPERCASE)
    for (int i = 0; display_name[i]; i++) {
        display_name[i] = toupper((unsigned char)display_name[i]);
    }
#endif

    lv_label_set_text(widget->obj, display_name);
}

static void layer_indicator_update_cb(struct layer_indicator_state state) {
#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
    /* The demo owns the wheel */
    ARG_UNUSED(state);
#else
    struct zmk_widget_layer_indicator *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        layer_indicator_set_sel(widget, state);
    }
#endif
}

#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
/*
 * Radii has no WPM animation, so its demo turns the wheel through the
 * layers instead. An LVGL timer, so it runs on the display thread.
 */
#define LAYER_DEMO_STEP_MS 2500

static void layer_demo_cb(lv_timer_t *timer) {
    static uint8_t index;
    ARG_UNUSED(timer);
    index = (index + 1) % ZMK_KEYMAP_LAYERS_LEN;
    struct zmk_widget_layer_indicator *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        layer_indicator_set_sel(widget, (struct layer_indicator_state){.index = index});
    }
}
#endif

static struct layer_indicator_state layer_indicator_get_state(const zmk_event_t *eh) {
    uint8_t index = zmk_keymap_highest_layer_active();
    return (struct layer_indicator_state){
        .index = index,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_indicator, struct layer_indicator_state, layer_indicator_update_cb,
                            layer_indicator_get_state)
ZMK_SUBSCRIPTION(widget_layer_indicator, zmk_layer_state_changed);

int zmk_widget_layer_indicator_init(struct zmk_widget_layer_indicator *widget, lv_obj_t *parent) {
    widget->container = lv_obj_create(parent);
    lv_obj_set_size(widget->container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(widget->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(widget->container, 0, 0);
    lv_obj_set_style_pad_all(widget->container, 0, 0);
    /* Wheel beside the name, both centred on the tile's height */
    lv_obj_set_flex_flow(widget->container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widget->container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(widget->container, 12, 0);

    // The wheel: a canvas redrawn at each angle, ARGB8888 so the tile shows through
    static uint8_t canvas_buf[LV_CANVAS_BUF_SIZE(WHEEL_SIZE, WHEEL_SIZE, 32, 1)];
    widget->wheel = lv_canvas_create(widget->container);
    lv_canvas_set_buffer(widget->wheel, canvas_buf, WHEEL_SIZE, WHEEL_SIZE, LV_COLOR_FORMAT_ARGB8888);
    wheel_draw(widget->wheel, wheel_angle);

    // Layer name label
    widget->obj = lv_label_create(widget->container);
    lv_obj_set_style_text_font(widget->obj, &PPF_NarrowThin_64, 0);
    lv_obj_set_style_text_color(widget->obj, lv_color_hex(DISPLAY_COLOR_LAYER_TEXT), 0);
    /* To the tile's right edge less 14 px; a long name wraps to two lines */
    lv_obj_set_width(widget->obj, 150);
    lv_label_set_long_mode(widget->obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(widget->obj, "");

    sys_slist_append(&widgets, &widget->node);
    widget_layer_indicator_init();
#if IS_ENABLED(CONFIG_PROSPECTOR_DEMO_WPM)
    layer_indicator_set_sel(widget, (struct layer_indicator_state){.index = 0});
    lv_timer_create(layer_demo_cb, LAYER_DEMO_STEP_MS, NULL);
#endif
    return 0;
}

lv_obj_t *zmk_widget_layer_indicator_obj(struct zmk_widget_layer_indicator *widget) {
    return widget->container;
}
