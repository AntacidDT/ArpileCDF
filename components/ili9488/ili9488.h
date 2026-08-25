#pragma once

#include <stdint.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ILI9488 3.5" 480x320 panel */
#define ILI9488_WIDTH  480
#define ILI9488_HEIGHT 320

#define ILI9488_PIN_NONE  (-1)

typedef struct {
    spi_host_device_t spi_host;  /* SPI2_HOST / SPI3_HOST */
    int gpio_mosi;               /* SPI data out (documented: GPIO5) */
    int gpio_sclk;               /* SPI clock (documented: GPIO21) */
    int gpio_cs;                 /* chip select (documented: GPIO23) */
    int gpio_dc;                 /* data/command select (documented: GPIO20) */
    int gpio_rst;                /* reset (documented: GPIO4) */
    int gpio_led;                /* backlight PWM pin, -1 if not used (documented: GPIO22) */
    int spi_clock_hz;            /* SPI clock, e.g. 20000000 */
    int spi_max_transfer_size;   /* max DMA chunk in bytes (0 = default 4096) */
} ili9488_config_t;

typedef struct ili9488_t ili9488_t;

/**
 * Allocate and initialise the display: installs the SPI bus (if not already
 * installed for this host), creates the SPI device, performs a hardware
 * reset + ILI9488 init sequence, and turns the backlight on.
 */
esp_err_t ili9488_create(const ili9488_config_t *cfg, ili9488_t **out_dev);

/** Free the device handle and release the SPI bus. */
esp_err_t ili9488_delete(ili9488_t *dev);

/** Enable/disable the backlight (100%/0% PWM duty; no-op if gpio_led == ILI9488_PIN_NONE). */
esp_err_t ili9488_backlight(ili9488_t *dev, bool on);

/** Set backlight brightness as a percentage of full PWM duty (0-100, clamped).
 *  Defaults to 100% after ili9488_create(). */
esp_err_t ili9488_backlight_level(ili9488_t *dev, int pct);

/** Fill a rectangle (x,y = top-left, w/h in pixels) with a 16-bit RGB565 color. */
esp_err_t ili9488_fill_rect(ili9488_t *dev, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, uint16_t color);

/** Fill the entire framebuffer with a single 16-bit RGB565 color. */
esp_err_t ili9488_fill_screen(ili9488_t *dev, uint16_t color);

/** Blit a sub-window of a packed row-major RGB565 image into the rectangle
 *  at (x,y,w,h). The source sub-window is (src_x, src_y, src_w, src_h) taken
 *  from an image with full row stride `img_w`. */
esp_err_t ili9488_draw_pixels(ili9488_t *dev, const uint16_t *rgb565,
                              uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              uint16_t src_x, uint16_t src_y,
                              uint16_t src_w, uint16_t src_h,
                              uint16_t img_w);

#ifdef __cplusplus
}
#endif
