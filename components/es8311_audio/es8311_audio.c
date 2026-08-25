#include "es8311_audio.h"

#include <math.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "arpile_audio";

/* Board: Waveshare ESP32-P4-NANO onboard ES8311 (from Waveshare wiki) */
#define AUDIO_I2C_NUM      0
#define AUDIO_I2C_SDA      7
#define AUDIO_I2C_SCL      8
#define AUDIO_I2S_NUM      0
#define AUDIO_I2S_MCLK     13
#define AUDIO_I2S_BCLK     12
#define AUDIO_I2S_WS       10
#define AUDIO_I2S_DIN      9   /* ESP -> codec DAC (DSDIN) */
#define AUDIO_I2S_DOUT     11  /* codec ADC -> ESP (ASDOUT, unused for tone) */
#define AUDIO_PA_PIN       53  /* power-amplifier enable, active high */
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_MCLK_MULT     384
#define AUDIO_VOLUME        60
#define TONE_2PI            6.28318530717958647692

static i2s_chan_handle_t     tx_handle   = NULL;
static i2s_chan_handle_t     rx_handle   = NULL;
static esp_codec_dev_handle_t codec_handle = NULL;
static int s_volume = AUDIO_VOLUME;

esp_err_t arpile_audio_init(void)
{
    /* --- I2S TX + RX channels (master) — match proven Espressif example --- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws   = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DIN,
            .din  = AUDIO_I2S_DOUT,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG, "i2s init std tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), TAG, "i2s init std rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG, "i2s enable TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_handle), TAG, "i2s enable RX");

    /* --- I2C bus for codec control --- */
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = AUDIO_I2C_NUM,
        .sda_io_num = AUDIO_I2C_SDA,
        .scl_io_num = AUDIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c_new_master_bus");

    audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
        .port = AUDIO_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "audio_codec_new_i2c_ctrl returned NULL");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = AUDIO_I2S_NUM,
        .tx_handle = tx_handle,
        .rx_handle = rx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio_codec_new_i2s_data returned NULL");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();  /* unused but required */
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "audio_codec_new_gpio returned NULL");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = (AUDIO_I2S_MCLK >= 0),
        .pa_pin = AUDIO_PA_PIN,
        .pa_reverted = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        .mclk_div = AUDIO_MCLK_MULT,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_if, ESP_FAIL, TAG, "es8311_codec_new returned NULL");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    codec_handle = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(codec_handle, ESP_FAIL, TAG, "esp_codec_dev_new returned NULL");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(codec_handle, s_volume) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_set_out_vol failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ES8311 initialized (I2C %d/%d, I2S MCLK%d BCLK%d WS%d DIN%d, PA%d)",
             AUDIO_I2C_SDA, AUDIO_I2C_SCL, AUDIO_I2S_MCLK, AUDIO_I2S_BCLK,
             AUDIO_I2S_WS, AUDIO_I2S_DIN, AUDIO_PA_PIN);
    return ESP_OK;
}

esp_err_t arpile_audio_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    if (codec_handle == NULL || tx_handle == NULL) {
        ESP_LOGE(TAG, "play_tone called before arpile_audio_init");
        return ESP_ERR_INVALID_STATE;
    }

    const int sr = AUDIO_SAMPLE_RATE;
    /* Build a 1-second mono sine buffer, then interleave to stereo. */
    int16_t *mono = malloc((size_t)sr * sizeof(int16_t));
    int16_t *stereo = malloc((size_t)sr * 2 * sizeof(int16_t));
    if (mono == NULL || stereo == NULL) {
        free(mono);
        free(stereo);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < sr; i++) {
        mono[i] = (int16_t)(sin(TONE_2PI * freq_hz * i / sr) * 28000);
    }
    for (int i = 0; i < sr; i++) {
        stereo[2 * i]     = mono[i];
        stereo[2 * i + 1] = mono[i];
    }
    free(mono);

    size_t bytes_per_sec = (size_t)sr * 2 * sizeof(int16_t);
    ESP_LOGI(TAG, "playing %.0f Hz tone for %u ms", (double)freq_hz, duration_ms);

    uint32_t elapsed = 0;
    while (elapsed < duration_ms) {
        size_t written = 0;
        esp_err_t r = i2s_channel_write(tx_handle, stereo, bytes_per_sec, &written, portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(r));
            break;
        }
        elapsed += 1000; /* one buffer == ~1 s at this sample rate */
    }

    free(stereo);
    return ESP_OK;
}


bool arpile_audio_ready(void)
{
    return codec_handle != NULL && tx_handle != NULL;
}

esp_err_t arpile_audio_write_pcm(const int16_t *stereo_interleaved,
                                 size_t sample_pairs)
{
    if (codec_handle == NULL || tx_handle == NULL) {
        ESP_LOGE(TAG, "write_pcm called before arpile_audio_init");
        return ESP_ERR_INVALID_STATE;
    }
    if (!stereo_interleaved || sample_pairs == 0) return ESP_ERR_INVALID_ARG;

    size_t bytes = sample_pairs * 2 * sizeof(int16_t);
    const uint8_t *p = (const uint8_t *)stereo_interleaved;
    while (bytes > 0) {
        size_t written = 0;
        esp_err_t r = i2s_channel_write(tx_handle, p, bytes, &written,
                                        portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(r));
            return r;
        }
        p += written;
        bytes -= written;
    }
    return ESP_OK;
}

void arpile_audio_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (codec_handle)
        esp_codec_dev_set_out_vol(codec_handle, s_volume);
}

int arpile_audio_get_volume(void)
{
    return s_volume;
}

void arpile_audio_deinit(void)
{
    if (codec_handle) {
        esp_codec_dev_close(codec_handle);
        codec_handle = NULL;
    }
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
}
