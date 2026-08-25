idf_component_register(SRCS "sd_card.c"
                    INCLUDE_DIRS "."
                    REQUIRES sdmmc fatfs esp_driver_gpio)