#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arpile UI core: minimal color/geometry primitives. Everything is drawn by
 * the ILI9488 driver via incremental fill_rect (no full framebuffer), so each
 * widget redraws only its own region. Colors are RGB565.
 */

#define UI_W   480
#define UI_H   320

/* Panel toolbar / shell height; windows live above it. */
#define UI_PANEL_H  20

typedef struct { uint16_t x, y, w, h; } ui_rect_t;

static inline ui_rect_t ui_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    ui_rect_t r = { x, y, w, h };
    return r;
}

/* Is point inside rect? */
static inline bool ui_rect_contains(const ui_rect_t *r, int16_t px, int16_t py)
{
    return (px >= r->x && px < (int16_t)(r->x + r->w) &&
            py >= r->y && py < (int16_t)(r->y + r->h));
}

/* Clip src against dst -> result (intersection), false if empty. */
static inline bool ui_rect_clip(const ui_rect_t *src, const ui_rect_t *dst,
                                ui_rect_t *out)
{
    int16_t x0 = src->x > dst->x ? src->x : dst->x;
    int16_t y0 = src->y > dst->y ? src->y : dst->y;
    int16_t x1 = (int16_t)(src->x + src->w) < (int16_t)(dst->x + dst->w)
                     ? (int16_t)(src->x + src->w) : (int16_t)(dst->x + dst->w);
    int16_t y1 = (int16_t)(src->y + src->h) < (int16_t)(dst->y + dst->h)
                     ? (int16_t)(src->y + src->h) : (int16_t)(dst->y + dst->h);
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    out->x = x0; out->y = y0; out->w = (uint16_t)(x1 - x0);
    out->h = (uint16_t)(y1 - y0);
    return true;
}

typedef struct {
    uint16_t r, g, b;
} ui_color_t;

#define UI_RGB(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)))

/* Dark Breeze-like palette (RGB565) */
#define UI_C_BG          UI_RGB(0x20, 0x36, 0x4E)  /* desktop fallback */
#define UI_C_PANEL       UI_RGB(0x2E, 0x50, 0x72)  /* panel */
#define UI_C_PANEL_HI    UI_RGB(0x4A, 0x6E, 0x91)
#define UI_C_PANEL_TEXT  UI_RGB(0xEF, 0xF0, 0xF1)  /* light text on dark panel */
#define UI_C_WIN_BG      UI_RGB(0x20, 0x36, 0x4E)  /* dark window face */
#define UI_C_WIN_TITLE   UI_RGB(0x28, 0x48, 0x6F)  /* active title */
#define UI_C_WIN_TITLE_I UI_RGB(0x5A, 0x7C, 0x9E)  /* inactive title */
#define UI_C_TEXT        UI_RGB(0xEF, 0xF0, 0xF1)  /* light text on dark face */
#define UI_C_TEXT_DIM    UI_RGB(0xAF, 0xBD, 0xC9)  /* secondary text */
#define UI_C_TEXT_LIGHT  UI_RGB(0xEF, 0xF0, 0xF1)
#define UI_C_ACCENT      UI_RGB(0x2A, 0x99, 0xE8)
#define UI_C_BORDER      UI_RGB(0x4A, 0x6E, 0x91)
#define UI_C_TASK_ACT    UI_RGB(0x2A, 0x99, 0xE8)
#define UI_C_BUTTON      UI_RGB(0x4A, 0x6E, 0x91)  /* dark button fill */
#define UI_C_BUTTON_DOWN UI_RGB(0x2E, 0x50, 0x72)
#define UI_C_RED      UI_RGB(0xFF, 0x00, 0x00)  /* red */

#ifdef __cplusplus
}
#endif