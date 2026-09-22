#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

/*
 * Grid for the 428x142 NV3007 canvas: 12 x 4 cells at the original 34 px
 * spacing, which is the same 48 cells the 280x240 layout used. The widget
 * fills the whole screen so that the modifier column, which is positioned
 * relative to the screen centre, lines up with the grid.
 */
#define LINE_SEGMENTS_GRID_COLS 12
#define LINE_SEGMENTS_GRID_ROWS 4
#define LINE_SEGMENTS_SPACING 34

/* Centre of the first cell; the grid is centred in the widget on each axis */
#define LINE_SEGMENTS_GRID_OFFSET_X 27
#define LINE_SEGMENTS_GRID_OFFSET_Y 20

#define LINE_SEGMENTS_WIDTH 428
#define LINE_SEGMENTS_HEIGHT 142

#define LINE_SEGMENTS_CELL_X(col) (LINE_SEGMENTS_GRID_OFFSET_X + (col) * LINE_SEGMENTS_SPACING)
#define LINE_SEGMENTS_CELL_Y(row) (LINE_SEGMENTS_GRID_OFFSET_Y + (row) * LINE_SEGMENTS_SPACING)

/* Rows the labels occupy, from the top */
#define LINE_SEGMENTS_LAYER_ROW 0
#define LINE_SEGMENTS_OUTPUT_ROW 1
#define LINE_SEGMENTS_BATTERY_ROW (LINE_SEGMENTS_GRID_ROWS - 1)

struct zmk_widget_line_segments {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *layer_label;
    lv_obj_t *battery_label;
    lv_obj_t *output;
};

int zmk_widget_line_segments_init(struct zmk_widget_line_segments *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_line_segments_obj(struct zmk_widget_line_segments *widget);

// Set the labels to exclude from line drawing
void zmk_widget_line_segments_set_labels(struct zmk_widget_line_segments *widget,
                                         lv_obj_t *layer_label,
                                         lv_obj_t *battery_label,
                                         lv_obj_t *output);

// Exclude/include a specific grid cell (for fixed-position elements like modifiers)
void zmk_widget_line_segments_set_cell_excluded(int col, int row, bool excluded);
