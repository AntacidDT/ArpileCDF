idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ili9488 lcd_text es8311_audio usb_host_input sd_card file_xfer arpile_ui wifi_test esp_wifi esp_event esp_netif nvs_flash esp_driver_spi esp_driver_gpio)