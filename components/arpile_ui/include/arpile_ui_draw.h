#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "arpile_ui_core.h"
#include "ili9488.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Low-level drawing ops targeting a live ili9488 device. Colors are RGB565.
 * Each op clips to the display and draws incrementally (no framebuffer). */

/* Clip all subsequent draws to this region until ui_set_compositor_clip(NULL)
 * is called. Used by the window manager to repaint only a dirty area. */
void ui_set_compositor_clip(const ui_rect_t *clip);

/* Return the current compositor clip rect, or NULL if none. */
const ui_rect_t *ui_get_compositor_clip(void);

/* Software compositing framebuffer. Writers go into RAM; call ui_fb_flush to
 * push the (single, atomic) dirty region to the LCD. */
void ui_fb_clear(uint16_t color);
void ui_fb_blit(const uint16_t *img, uint16_t x, uint16_t y,
                uint16_t w, uint16_t h,
                uint16_t src_x, uint16_t src_y, uint16_t src_w, uint16_t src_h,
                uint16_t img_w);
void ui_fb_write_rgb666(const uint8_t *src, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h);
void ui_fb_flush(ili9488_t *lcd, const ui_rect_t *r);

void ui_draw_fill_rect(ili9488_t *lcd, const ui_rect_t *r, uint16_t color);
void ui_draw_outline(ili9488_t *lcd, const ui_rect_t *r, uint16_t color);
void ui_draw_hline(ili9488_t *lcd, uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void ui_draw_vline(ili9488_t *lcd, uint16_t x, uint16_t y, uint16_t h, uint16_t color);

/* Inter proportional bitmap font (height ARPILE_FONT_H). */
#define CHAR_W 10
#define CHAR_H 14

uint16_t ui_font_height(void);
uint16_t ui_text_width(const char *s);
uint16_t ui_char_width(char c);

/* Text via the Inter bitmap font. Returns x just after last char. Glyphs are
 * topped at `y` (no descender baseline handling; font is fixed-height). */
uint16_t ui_draw_text(ili9488_t *lcd, uint16_t x, uint16_t y,
                      const char *s, uint16_t fg, uint16_t bg);

/* Render a 16x16 1-bit Material Symbols icon by name. Returns true if found. */
bool ui_draw_icon(ili9488_t *lcd, const char *name,
                  uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);
bool ui_draw_icon_scaled(ili9488_t *lcd, const char *name,
                         uint16_t x, uint16_t y, uint8_t scale,
                         uint16_t fg, uint16_t bg);
bool ui_draw_icon_img(ili9488_t *lcd, const char *name,
                      uint16_t x, uint16_t y, uint16_t size);

#ifdef __cplusplus
}
#endif