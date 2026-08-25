#include "ili9488.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9488";

/* Backlight PWM: fixed 10 kHz carrier, 10-bit resolution, duty drives
 * brightness percentage directly (1023 = 100%). */
#define ILI9488_BL_FREQ_HZ   10000
#define ILI9488_BL_RES       LEDC_TIMER_10_BIT
#define ILI9488_BL_DUTY_MAX  ((1 << 10) - 1)

struct ili9488_t {
    spi_device_handle_t spi;
    spi_host_device_t   host;
    int gpio_dc;
    int gpio_led;
    int chunk_size;   /* max bytes per SPI transaction */
};

/* ------------------------------------------------------------------ */
/* Low-level SPI transfers (4-wire: DC pin toggled per byte group)     */
/* ------------------------------------------------------------------ */

static esp_err_t ili9488_write(ili9488_t *dev, const uint8_t *data,
                               size_t len, bool is_cmd)
{
    if (len == 0) {
        return ESP_OK;
    }
    gpio_set_level(dev->gpio_dc, is_cmd ? 0 : 1);

    spi_transaction_t t = { 0 };
    t.length   = len * 8;          /* bits */
    t.tx_buffer = data;
    return spi_device_transmit(dev->spi, &t);
}

static esp_err_t ili9488_write_cmd(ili9488_t *dev, uint8_t cmd)
{
    return ili9488_write(dev, &cmd, 1, true);
}

static esp_err_t ili9488_write_data(ili9488_t *dev, const uint8_t *data, size_t len)
{
    return ili9488_write(dev, data, len, false);
}

/* ------------------------------------------------------------------ */
/* ILI9488 init sequence — adapted from the proven Espelt32 driver     */
/* (same MCU + ILI9488 panel). Uses 18-bit RGB666 pixel format.         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t cmd;
    uint8_t datalen;
    const uint8_t *data;
    uint16_t delay_ms;
} ili9488_cmd_t;

static const uint8_t d_e0[] = { 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
                                0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F };
static const uint8_t d_e1[] = { 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
                                0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F };
static const uint8_t d_c0[] = { 0x17, 0x15 };
static const uint8_t d_c1[] = { 0x41 };
static const uint8_t d_c2[] = { 0x44 };
static const uint8_t d_c5[] = { 0x00, 0x12, 0x80 };
static const uint8_t d_madctl[] = { 0x28 };  /* rotation 0, RGB order */
static const uint8_t d_pixfmt[] = { 0x66 };  /* 18-bit RGB666 (3 bytes/pixel) */
static const uint8_t d_b0[] = { 0x00 };
static const uint8_t d_b1[] = { 0xA0 };
static const uint8_t d_b4[] = { 0x02 };
static const uint8_t d_b6[] = { 0x02, 0x02 };
static const uint8_t d_e9[] = { 0x00 };
static const uint8_t d_53[] = { 0x2C };
static const uint8_t d_51[] = { 0xFF };
static const uint8_t d_f7[] = { 0xA9, 0x51, 0x2C, 0x02 };

static const ili9488_cmd_t ili9488_init_seq[] = {
    { 0x01, 0, NULL,       100 },  /* Software reset */
    { 0x11, 0, NULL,       120 },  /* Sleep out */
    { 0xE0, 15, d_e0,        0 },  /* Positive gamma */
    { 0xE1, 15, d_e1,        0 },  /* Negative gamma */
    { 0xC0, 2, d_c0,         0 },  /* Power control 1 */
    { 0xC1, 1, d_c1,         0 },  /* Power control 2 */
    { 0xC2, 1, d_c2,         0 },  /* Power control 3 */
    { 0xC5, 3, d_c5,         0 },  /* VCOM control */
    { 0x36, 1, d_madctl,     0 },  /* Memory access control */
    { 0x3A, 1, d_pixfmt,     0 },  /* Pixel format: 18-bit RGB666 */
    { 0xB0, 1, d_b0,         0 },  /* Interface mode control */
    { 0xB1, 1, d_b1,         0 },  /* Frame rate control */
    { 0xB4, 1, d_b4,         0 },  /* Display inversion control */
    { 0xB6, 2, d_b6,         0 },  /* Display function control */
    { 0xE9, 1, d_e9,         0 },  /* Set image function */
    { 0x53, 1, d_53,         0 },  /* Write CTRL display */
    { 0x51, 1, d_51,         0 },  /* Write brightness */
    { 0xF7, 4, d_f7,         0 },  /* Adjust control 3 */
    { 0x29, 0, NULL,        50 },  /* Display on */
};

/* Expand a 16-bit RGB565 color into 3 RGB666 bytes (ILI9488 18-bit mode). */
static void rgb565_to_rgb666(uint16_t color, uint8_t *out3)
{
    uint8_t r5 = (color >> 11) & 0x1F;
    uint8_t g6 = (color >> 5)  & 0x3F;
    uint8_t b5 = color & 0x1F;
    out3[0] = ((r5 << 1) | (r5 >> 4)) & 0xFC;
    out3[1] = g6 & 0xFC;
    out3[2] = ((b5 << 1) | (b5 >> 4)) & 0xFC;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t ili9488_create(const ili9488_config_t *cfg, ili9488_t **out_dev)
{
    if (cfg == NULL || out_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ili9488_t *dev = calloc(1, sizeof(ili9488_t));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    dev->host      = cfg->spi_host;
    dev->gpio_dc   = cfg->gpio_dc;
    dev->gpio_led  = cfg->gpio_led;
    dev->chunk_size = cfg->spi_max_transfer_size > 0
                          ? cfg->spi_max_transfer_size : 4096;

    /* DC + RST as outputs */
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << cfg->gpio_dc) | (1ULL << cfg->gpio_rst),
    };
    if (cfg->gpio_led != ILI9488_PIN_NONE) {
        io.pin_bit_mask |= (1ULL << cfg->gpio_led);
    }
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config failed");

    /* Hardware reset pulse (active low, 50ms) */
    gpio_set_level(cfg->gpio_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(cfg->gpio_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(cfg->gpio_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* SPI bus (install only once per host) */
    spi_bus_config_t buscfg = {
        .mosi_io_num = cfg->gpio_mosi,
        .miso_io_num = -1,            /* display is write-only */
        .sclk_io_num = cfg->gpio_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = dev->chunk_size,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    esp_err_t err = spi_bus_initialize(cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized for host %d", cfg->spi_host);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        free(dev);
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = cfg->spi_clock_hz,
        .mode = 0,
        .spics_io_num = cfg->gpio_cs,
        .queue_size = 8,
        .flags = SPI_DEVICE_NO_DUMMY,
        .pre_cb = NULL,
        .post_cb = NULL,
    };
    err = spi_bus_add_device(cfg->spi_host, &devcfg, &dev->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(cfg->spi_host);
        free(dev);
        return err;
    }

    /* Run init sequence (continue past any single non-fatal error) */
    for (size_t i = 0; i < sizeof(ili9488_init_seq) / sizeof(ili9488_init_seq[0]); i++) {
        const ili9488_cmd_t *c = &ili9488_init_seq[i];
        esp_err_t e = ili9488_write_cmd(dev, c->cmd);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "init cmd 0x%02X failed: %s", c->cmd, esp_err_to_name(e));
        }
        if (c->datalen > 0) {
            e = ili9488_write_data(dev, c->data, c->datalen);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "init data 0x%02X failed: %s", c->cmd, esp_err_to_name(e));
            }
        }
        if (c->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    *out_dev = dev;

    /* Backlight: LEDC channel on gpio_led, starts at 100% duty */
    if (dev->gpio_led != ILI9488_PIN_NONE) {
        ledc_timer_config_t timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .timer_num       = LEDC_TIMER_0,
            .duty_resolution = ILI9488_BL_RES,
            .freq_hz         = ILI9488_BL_FREQ_HZ,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        esp_err_t e = ledc_timer_config(&timer);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "backlight timer config failed: %s", esp_err_to_name(e));
            return e;
        }
        ledc_channel_config_t ch = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_0,
            .timer_sel  = LEDC_TIMER_0,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = dev->gpio_led,
            .duty       = ILI9488_BL_DUTY_MAX,   /* 100% */
            .hpoint     = 0,
        };
        e = ledc_channel_config(&ch);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "backlight channel config failed: %s", esp_err_to_name(e));
            return e;
        }
    }
    return ESP_OK;
}

esp_err_t ili9488_delete(ili9488_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ili9488_backlight(dev, false);
    if (dev->spi) {
        spi_bus_remove_device(dev->spi);
    }
    spi_bus_free(dev->host);
    free(dev);
    return ESP_OK;
}

esp_err_t ili9488_backlight(ili9488_t *dev, bool on)
{
    return ili9488_backlight_level(dev, on ? 100 : 0);
}

esp_err_t ili9488_backlight_level(ili9488_t *dev, int pct)
{
    if (dev == NULL || dev->gpio_led == ILI9488_PIN_NONE) {
        return ESP_OK;
    }
    if (pct < 0)   { pct = 0; }
    if (pct > 100) { pct = 100; }
    uint32_t duty = ((uint32_t)ILI9488_BL_DUTY_MAX * (uint32_t)pct) / 100;
    esp_err_t e = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (e != ESP_OK) {
        return e;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t ili9488_fill_rect(ili9488_t *dev, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, uint16_t color)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= ILI9488_WIDTH || y >= ILI9488_HEIGHT) {
        return ESP_OK;
    }
    if (x + w > ILI9488_WIDTH)  { w = ILI9488_WIDTH  - x; }
    if (y + h > ILI9488_HEIGHT) { h = ILI9488_HEIGHT - y; }
    if (w == 0 || h == 0) {
        return ESP_OK;
    }

    /* Column address set (0x2A) and page address set (0x2B) */
    uint8_t ca[4] = { x >> 8, x & 0xFF, (x + w - 1) >> 8, (x + w - 1) & 0xFF };
    uint8_t pa[4] = { y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF };
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2A), TAG, "CA");
    ESP_RETURN_ON_ERROR(ili9488_write_data(dev, ca, 4), TAG, "CA data");
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2B), TAG, "PA");
    ESP_RETURN_ON_ERROR(ili9488_write_data(dev, pa, 4), TAG, "PA data");

    /* Memory write (0x2C) then stream pixel data.
       Pixel format is 18-bit RGB666 => 3 bytes per pixel. */
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2C), TAG, "RAMWR");

    uint8_t px[3];
    rgb565_to_rgb666(color, px);

    /* Build a reusable chunk buffer of whole pixels (3 bytes each) */
    uint16_t chunk_px = dev->chunk_size / 3;
    if (chunk_px == 0) {
        chunk_px = 1;
    }
    uint8_t *buf = malloc((size_t)chunk_px * 3);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t i = 0; i < chunk_px; i++) {
        buf[i * 3]     = px[0];
        buf[i * 3 + 1] = px[1];
        buf[i * 3 + 2] = px[2];
    }

    uint32_t remaining_px = (uint32_t)w * h;
    gpio_set_level(dev->gpio_dc, 1);  /* data mode for the whole stream */
    while (remaining_px > 0) {
        uint32_t send_px = remaining_px > chunk_px ? chunk_px : remaining_px;
        spi_transaction_t t = { 0 };
        t.length   = send_px * 3 * 8;
        t.tx_buffer = buf;
        ESP_RETURN_ON_ERROR(spi_device_transmit(dev->spi, &t), TAG, "pixel stream");
        remaining_px -= send_px;
    }

    free(buf);
    return ESP_OK;
}

/* Blit a sub-window of a packed row-major RGB565 image into a rect. */
esp_err_t ili9488_draw_pixels(ili9488_t *dev, const uint16_t *rgb565,
                              uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              uint16_t src_x, uint16_t src_y,
                              uint16_t src_w, uint16_t src_h,
                              uint16_t img_w)
{
    if (dev == NULL || rgb565 == NULL || img_w == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= ILI9488_WIDTH || y >= ILI9488_HEIGHT) {
        return ESP_OK;
    }
    if (x + w > ILI9488_WIDTH)  { w = ILI9488_WIDTH  - x; }
    if (y + h > ILI9488_HEIGHT) { h = ILI9488_HEIGHT - y; }
    if (w == 0 || h == 0 || src_w == 0 || src_h == 0) {
        return ESP_OK;
    }

    uint8_t ca[4] = { x >> 8, x & 0xFF, (x + w - 1) >> 8, (x + w - 1) & 0xFF };
    uint8_t pa[4] = { y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF };
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2A), TAG, "CA");
    ESP_RETURN_ON_ERROR(ili9488_write_data(dev, ca, 4), TAG, "CA data");
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2B), TAG, "PA");
    ESP_RETURN_ON_ERROR(ili9488_write_data(dev, pa, 4), TAG, "PA data");
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2C), TAG, "RAMWR");

    static const uint16_t chunk_px = 256;
    uint8_t buf[chunk_px * 3];
    uint32_t remaining_px = (uint32_t)w * h;
    gpio_set_level(dev->gpio_dc, 1);

    uint16_t dst_row = 0, dst_col = 0;
    while (remaining_px > 0) {
        uint16_t send = remaining_px > chunk_px ? chunk_px : (uint16_t)remaining_px;
        for (uint16_t i = 0; i < send; i++) {
            uint32_t sx = src_x + (uint32_t)(dst_col) * src_w / w;
            uint32_t sy = src_y + (uint32_t)(dst_row) * src_h / h;
            if (sx >= (uint32_t)src_x + src_w) sx = (uint32_t)src_x + src_w - 1;
            if (sy >= (uint32_t)src_y + src_h) sy = (uint32_t)src_y + src_h - 1;
            uint16_t color = rgb565[sy * img_w + sx];
            uint8_t r5 = (color >> 11) & 0x1F;
            uint8_t g6 = (color >> 5)  & 0x3F;
            uint8_t b5 = color & 0x1F;
            buf[i * 3]     = ((r5 << 1) | (r5 >> 4)) & 0xFC;
            buf[i * 3 + 1] = g6 & 0xFC;
            buf[i * 3 + 2] = ((b5 << 1) | (b5 >> 4)) & 0xFC;

            dst_col++;
            if (dst_col >= w) {
                dst_col = 0;
                dst_row++;
            }
        }
        spi_transaction_t t = { 0 };
        t.length   = send * 3 * 8;
        t.tx_buffer = buf;
        ESP_RETURN_ON_ERROR(spi_device_transmit(dev->spi, &t), TAG, "pixels");
        remaining_px -= send;
    }
    return ESP_OK;
}

esp_err_t ili9488_fill_screen(ili9488_t *dev, uint16_t color)
{
    return ili9488_fill_rect(dev, 0, 0, ILI9488_WIDTH, ILI9488_HEIGHT, color);
}

/* DOOM integration: pipelined raw RGB666 streaming blit (additive). */
esp_err_t ili9488_blit_rgb666_stream(ili9488_t *dev, const uint8_t *rgb666,
                                     uint16_t x, uint16_t y,
                                     uint16_t w, uint16_t h)
{
    if (dev == NULL || rgb666 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= ILI9488_WIDTH || y >= ILI9488_HEIGHT || w == 0 || h == 0) {
        return ESP_OK;
    }

    uint8_t ca[4] = { x >> 8, x & 0xFF, (x + w - 1) >> 8, (x + w - 1) & 0xFF };
    uint8_t pa[4] = { y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF };
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2A), TAG, "CA");
    ESP_RETURN_ON_ERROR(ili9488_write_data(dev, ca, 4), TAG, "CA data");
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2B), TAG, "PA");
    ESP_RETURN_ON_ERROR(ili9488_write_data(dev, pa, 4), TAG, "PA data");
    ESP_RETURN_ON_ERROR(ili9488_write_cmd(dev, 0x2C), TAG, "RAMWR");

    const size_t chunk_bytes = 3840;             /* 1280 px, <= max_transfer */
    const size_t total = (size_t)w * h * 3;
    const int INFLIGHT = 8;                      /* matches device queue_size */

    size_t off = 0;
    int inflight = 0;
    gpio_set_level(dev->gpio_dc, 1);
    while (off < total || inflight > 0) {
        if (off < total && inflight < INFLIGHT) {
            size_t len = total - off;
            if (len > chunk_bytes) len = chunk_bytes;
            spi_transaction_t *t = calloc(1, sizeof(*t));
            if (t == NULL) return ESP_ERR_NO_MEM;
            t->length = len * 8;
            t->tx_buffer = rgb666 + off;
            t->user = NULL;
            esp_err_t e = spi_device_queue_trans(dev->spi, t, portMAX_DELAY);
            if (e != ESP_OK) { free(t); return e; }
            off += len;
            inflight++;
        } else {
            spi_transaction_t *rt;
            esp_err_t e = spi_device_get_trans_result(dev->spi, &rt, portMAX_DELAY);
            free(rt);
            if (e != ESP_OK) return e;
            inflight--;
        }
    }
    return ESP_OK;
}
