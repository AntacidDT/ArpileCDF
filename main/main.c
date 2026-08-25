#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ili9488.h"
#include "es8311_audio.h"
#include "usb_host_input.h"
#include "sd_card.h"
#include "file_xfer.h"
#include "arpile_ui.h"
#include "wifi_test.h"

static const char *TAG = "arpile_32cdf";

/* Arpile 32CDF — ILI9488 wiring (from ArpileDocumentation) */
#define LCD_HOST        SPI2_HOST
#define PIN_LCD_MOSI    5
#define PIN_LCD_SCLK    21
#define PIN_LCD_CS      23
#define PIN_LCD_DC      20
#define PIN_LCD_RST     4
#define PIN_LCD_LED     22

/* Boot blink: confirms code is running (no serial console over USB on this
   board). Only used on the LCD failure path now that boot is fast. */
static void blink_backlight(int pin, int times, int ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(pin, 0);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(pin, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

/* Deferred peripheral bring-up: runs while the desktop is already visible.
 * Order preserved: audio -> usb -> wifi -> sd (WiFi must claim the C6 radio
 * / SDMMC slot before the TF card mounts). */
static void late_init_task(void *arg)
{
    esp_err_t aerr = arpile_audio_init();
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 audio init FAILED (0x%x)", aerr);
    }

    esp_err_t uerr = arpile_usb_host_init();
    if (uerr != ESP_OK) {
        ESP_LOGE(TAG, "USB Host init FAILED (0x%x)", uerr);
    }

    ESP_LOGI(TAG, "WiFi init: stack starting...");
    esp_err_t werr = arpile_wifi_init();
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init FAILED (0x%x)", werr);
    }

    esp_err_t serr = arpile_sd_init();
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "SD card init FAILED (0x%x)", serr);
    }

    /* TEMP DIAGNOSTIC: file_xfer disabled - suspected freeze trigger */
    // arpile_file_xfer_start();

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Arpile 32CDF firmware starting");

    /* TEMP DIAGNOSTIC: visual boot progress markers (console unreliable) */
    blink_backlight(PIN_LCD_LED, 1, 150);

    /* Backlight off until the ILI9488 driver owns it (LED PWM at 100%). */
    gpio_reset_pin(PIN_LCD_LED);
    gpio_set_direction(PIN_LCD_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_LED, 0);

    ili9488_config_t cfg = {
        .spi_host = LCD_HOST,
        .gpio_mosi = PIN_LCD_MOSI,
        .gpio_sclk = PIN_LCD_SCLK,
        .gpio_cs   = PIN_LCD_CS,
        .gpio_dc   = PIN_LCD_DC,
        .gpio_rst  = PIN_LCD_RST,
        .gpio_led  = PIN_LCD_LED,
        .spi_clock_hz = 80000000,   /* DOOM: approved bump */
        .spi_max_transfer_size = 4096,
    };

    ili9488_t *lcd = NULL;
    esp_err_t err = ili9488_create(&cfg, &lcd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ILI9488 bring-up failed: %s", esp_err_to_name(err));
        blink_backlight(PIN_LCD_LED, 6, 120);
        return;
    }
    ESP_LOGI(TAG, "ILI9488 initialized");
    blink_backlight(PIN_LCD_LED, 2, 150);   /* TEMP DIAG: LCD OK */

    /* Splash: paint the desktop background colour immediately so the panel
     * never sits blank/white while the blocking init steps below run. */
    ui_fb_clear(UI_C_BG);
    ui_rect_t full = { 0, 0, UI_W, UI_H };
    ui_fb_flush(lcd, &full);

    /* Desktop first: UI task is up and compositing while peripherals init. */
    err = arpile_ui_start(lcd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Arpile desktop start FAILED (0x%x)", err);
        return;
    }

    if (xTaskCreate(late_init_task, "late_init", 16384, NULL, 5, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "late_init task spawn FAILED");
    }

    /* app_main returns; UI task + late_init continue to run. */
    vTaskDelete(NULL);
}
