idf_component_register(SRCS "ili9488.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_spi esp_driver_gpio esp_driver_ledc)
