#include "arpile_imgdec.h"

#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "driver/jpeg_decode.h"

uint16_t *arpile_jpeg_decode_hw(const uint8_t *jpg, size_t jpg_len,
                                int max_w, int max_h,
                                int hard_cap_w, int hard_cap_h,
                                int *out_w, int *out_h)
{
    *out_w = 0;
    *out_h = 0;
    if (!jpg || jpg_len < 4)
        return NULL;

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg, jpg_len, &info) != ESP_OK)
        return NULL;
    if (info.width == 0 || info.height == 0 ||
        info.width > (uint32_t)hard_cap_w || info.height > (uint32_t)hard_cap_h)
        return NULL;

    jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 3000 };
    jpeg_decoder_handle_t dec = NULL;
    if (jpeg_new_decoder_engine(&eng, &dec) != ESP_OK)
        return NULL;

    uint16_t *result = NULL;
    size_t raw_sz = (size_t)info.width * info.height * 2u;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = { 0 };
    uint8_t *raw = jpeg_alloc_decoder_mem(raw_sz, &mem_cfg, NULL);

    if (raw) {
        jpeg_decode_cfg_t dcfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        if (jpeg_decoder_process(dec, &dcfg, jpg, jpg_len,
                                 raw, raw_sz, &out_size) == ESP_OK) {
            float s = 1.0f;
            if ((float)info.width > max_w) s = (float)max_w / info.width;
            if ((float)info.height * s > max_h) s = (float)max_h / info.height;
            int dw = (int)(info.width * s);
            if (dw < 1) dw = 1;
            int dh = (int)(info.height * s);
            if (dh < 1) dh = 1;

            result = malloc((size_t)dw * dh * sizeof(uint16_t));
            if (result) {
                const uint16_t *src16 = (const uint16_t *)raw;
                for (int y = 0; y < dh; y++) {
                    const uint16_t *src =
                        src16 + (size_t)((uint64_t)y * info.height / dh) * info.width;
                    uint16_t *dst = result + (size_t)y * dw;
                    for (int x = 0; x < dw; x++)
                        dst[x] = src[(uint32_t)((uint64_t)x * info.width / dw)];
                }
                *out_w = dw;
                *out_h = dh;
            }
        }
        free(raw); /* allocated via jpeg_alloc_decoder_mem */
    }

    jpeg_del_decoder_engine(dec);
    return result;
}
