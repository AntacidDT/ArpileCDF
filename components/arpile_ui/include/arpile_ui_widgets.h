#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "arpile_ui_core.h"
#include "arpile_ui_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reusable widget renderers (pure draw: no persistent widget state). Apps can
 * call these directly inside their window on_paint. All rects are in window
 * client coordinates. */

typedef struct {
    ui_rect_t rect;
    const char *label;
    bool down;      /* visually pressed */
    bool focused;   /* keyboard focus highlight */
} ui_button_t;

void ui_paint_button(ili9488_t *lcd, const ui_button_t *b);

typedef struct {
    ui_rect_t rect;
    const char *title;
} ui_panel_t;

void ui_paint_panel(ili9488_t *lcd, const ui_panel_t *p);      /* titled panel */
void ui_paint_group(ili9488_t *lcd, const ui_rect_t *r);        /* bordered group */

typedef struct {
    int left, top, right, bottom;   /* viewport in client coords */
    int view_w, view_h;             /* logical size of scrolled content */
    int scroll_y;                   /* scroll offset */
} ui_scroll_t;

void ui_paint_scroll_edges(ili9488_t *lcd, const ui_scroll_t *s);

/* Simple list: paint rows[count], highlight row `sel`, clip to rect. */
void ui_paint_list(ili9488_t *lcd, const ui_rect_t *r,
                   const char *const *rows, int count, int sel);

/* Dialog frame: panel + title bar + optional buttons region. Draws a modal
 * box centered-ish at (x,y,w,h); caller paints content afterwards. */
typedef struct {
    ui_rect_t rect;
    const char *title;
} ui_dialog_t;

void ui_paint_dialog(ili9488_t *lcd, const ui_dialog_t *d, const char *message,
                     const char *btn_ok, const char *btn_cancel);

/* ---------------- app kit: shared navigation + chrome ---------------- */

/* Pure list-navigation logic shared by all list-based apps. Feeds a nav key
 * (ARPILE_KEY_*), moves *sel through count items and keeps the window
 * starting at *top showing view_rows rows. Returns true if state changed. */
bool ui_kit_list_nav(int *sel, int *top, int view_rows, int count,
                     uint16_t keycode);

/* Paint a scrollable list using a label callback. Rows are drawn from
 * *top, highlighting absolute row sel. row_label returns NULL for blank. */
typedef const char *(*ui_kit_row_fn)(int index, void *user);
void ui_kit_list_paint(ili9488_t *lcd, const ui_rect_t *r, int row_h,
                       int top, int sel, int count,
                       ui_kit_row_fn get_label, void *user);

/* Horizontal tab bar; active tab drawn with accent fill. Returns the rect of
 * tab i (for hit-testing) via out_tab_i if non-NULL. */
void ui_kit_tabbar(ili9488_t *lcd, const ui_rect_t *r,
                   const char *const *tabs, int count, int active);
bool ui_kit_tabbar_hit(const ui_rect_t *r, const char *const *tabs, int count,
                       int mx, int my, int *out_tab_i);

/* One-line status bar along the bottom of rect (text left, optional right). */
void ui_kit_statusbar(ili9488_t *lcd, const ui_rect_t *r,
                      const char *left, const char *right);

/* "Key: value" row at (x,y); returns y advance (CHAR_H + 4). */
int ui_kit_kv(ili9488_t *lcd, int x, int y, const char *key, const char *value);

/* Like ui_kit_kv but word-wraps long values within max_w pixels; returns
 * the new y after the wrapped block. */
int ui_kit_kv_wrap(ili9488_t *lcd, int x, int y, const char *key,
                   const char *value, int max_w);

/* Section header text with accent underline; returns y advance. */
int ui_kit_header(ili9488_t *lcd, int x, int y, const char *title);

#ifdef __cplusplus
}
#endif