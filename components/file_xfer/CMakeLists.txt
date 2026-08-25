idf_component_register(SRCS "file_xfer.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_uart esp_driver_usb_serial_jtag fatfs vfs esp_rom)
