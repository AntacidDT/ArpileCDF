#pragma once

#include "ili9488.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Simple 5x7 bitmap text renderer written on top of ili9488_fill_rect.
   Enough to echo USB host + HID events to the screen without serial. */

/** Draw a single character at (x,y) = top-left, in 5x7 px, fg/bg colors. */
void lcd_text_putc(ili9488_t *dev, uint16_t x, uint16_t y, char c,
                   uint16_t fg, uint16_t bg);

/** Draw a null-terminated string. Returns the x just after the last char. */
uint16_t lcd_text_puts(ili9488_t *dev, uint16_t x, uint16_t y, const char *s,
                       uint16_t fg, uint16_t bg);

/** Width in px of one character incl. 1px horizontal padding. */
#define LCD_TEXT_CHAR_W (6)   /* 5 wide + 1 spacing */
#define LCD_TEXT_CHAR_H (7)

#define LCD_TEXT_FG  0xFFFF
#define LCD_TEXT_BG  0x0000

#ifdef __cplusplus
}
#endif