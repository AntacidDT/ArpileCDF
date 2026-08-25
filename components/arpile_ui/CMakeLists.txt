idf_component_register(SRCS "arpile_app_caw.c" "arpile_app_media.c" "arpile_imgdec.c" "arpile_app_prayertimes.c" "arpile_app_sysmon.c" "arpile_app_wiki.c" "arpile_app_biology.c" "arpile_app_chemistry.c" "arpile_app_physics.c" "arpile_app_math.c" "arpile_app_code.c" "arpile_app_wifi.c" "arpile_app_settings.c" "arpile_app_files.c" "arpile_app_simple.c" "arpile_app_terminal.c" "arpile_app.c" "arpile_ui.c" "arpile_ui_draw.c" "arpile_ui_widgets.c" "arpile_freertos_stub.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ili9488 usb_host_input wifi_test es8311_audio esp_http_client cjson esp_driver_jpeg esp-tls
                             esp_partition esp_app_format esp_timer)
set(COMPONENT_CFLAGS "${COMPONENT_CFLAGS} -Wno-error=char-subscripts")