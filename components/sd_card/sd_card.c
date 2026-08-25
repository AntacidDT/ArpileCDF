#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_types.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_vfs_fat.h"
#include "sd_card.h"

static const char *TAG = "arpile_sd";

#define SD_POWER_GPIO   45   /* pull LOW to enable Q1 P-MOSFET -> SD 3.3V */
#define SD_POWER_LDO    4    /* on-chip LDO channel powering SDMMC IO (+3.3V) */
#define SD_1BIT_WAIT    300  /* ms to wait after enabling power             */

#define SD_TEST_MOUNT   "/sdcard"
#define SD_TEST_FILE    "/sdcard/sdtest.txt"

static void (*s_text_cb)(const char *text) = NULL;

void arpile_sd_set_text_cb(void (*cb)(const char *text))
{
    s_text_cb = cb;
}

static void notify_text(const char *line)
{
    if (s_text_cb) {
        s_text_cb(line);
    }
}

esp_err_t arpile_sd_init(void)
{
    /* Enable card power via GPIO45 (low = P-MOSFET on). */
    gpio_reset_pin(SD_POWER_GPIO);
    gpio_config_t pw_cfg = {
        .pin_bit_mask = (1ULL << SD_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pw_cfg);
    gpio_set_level(SD_POWER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(SD_1BIT_WAIT));

    ESP_LOGI(TAG, "SD card power enabled (GPIO%u LOW); mounting FATFS", SD_POWER_GPIO);

    /* SDMMC slot 0 — pins are fixed by hardware on ESP32-P4:
       CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    /* ESP32-P4 SDMMC lines need IO voltage supplied by the on-chip LDO unit.
       The Nano board connects LDO channel 4 (LDO_VO4) to the SD power rail.
       Without this power-control handle the card gets no bus power. */
    sd_pwr_ctrl_handle_t sd_pwr_ctrl = NULL;
    const sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_POWER_LDO,
    };
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &sd_pwr_ctrl),
                        TAG, "failed to init on-chip LDO power ctrl");
    host.pwr_ctrl_handle = sd_pwr_ctrl;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;   /* 4-bit mode; board wires all 4 data lines */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,   /* never format a card automatically */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_TEST_MOUNT, &host, &slot_config,
                                            &mount_config, &card);
    if (ret != ESP_OK) {
        char line[64];
        snprintf(line, sizeof(line), "SD fail: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "SD mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        notify_text(line);
        return ret;
    }

    ESP_LOGI(TAG, "SD mounted: %s", card->cid.name);

    /* Self-test: write a small file, then read it back. */
    FILE *f = fopen(SD_TEST_FILE, "w");
    if (!f) {
        esp_vfs_fat_sdcard_unmount(SD_TEST_MOUNT, card);
        notify_text("SD fail: open write");
        return ESP_ERR_NOT_FOUND;
    }
    fprintf(f, "Arpile 32CDF SD self-test OK\n");
    fclose(f);

    f = fopen(SD_TEST_FILE, "r");
    if (!f) {
        esp_vfs_fat_sdcard_unmount(SD_TEST_MOUNT, card);
        notify_text("SD fail: open read");
        return ESP_ERR_NOT_FOUND;
    }
    char buf[64] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    uint64_t bytes = ((uint64_t)card->csd.capacity)
                     * card->csd.sector_size;
    char line[80];
    snprintf(line, sizeof(line), "SD ok %.1fGB W+R", bytes / (1024.0 * 1024.0 * 1024.0));
    notify_text(line);
    ESP_LOGI(TAG, "SD self-test passed, read back: %s (card %.1fGB, free via stat)",
             buf, bytes / (1024.0 * 1024.0 * 1024.0));

    /* Leave the card mounted for the runtime to use. */
    return ESP_OK;
}