idf_component_register(SRCS "es8311_audio.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_codec_dev esp_driver_i2s esp_driver_i2c esp_driver_gpio)
