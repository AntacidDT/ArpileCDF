idf_component_register(SRCS
    "i_main.c" "i_system.c" "i_video.c" "i_sound.c" "i_network.c"
    "doom_input.c" "arpile_app_doom.c"
    INCLUDE_DIRS "include"
    WHOLE_ARCHIVE
    REQUIRES prboom ili9488 arpile_ui es8311_audio usb_host_input fatfs wear_levelling esp_mm esp_partition spi_flash heap)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
