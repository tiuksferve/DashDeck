idf_component_register(
    SRCS "main.c" "ui.c" "link.c" "net.c" "power.c" "keys.c" "imu.c" "audio.c" "ota.c" "store.c" "anim.c" "sd.c" "plot.c" "bg.c" "lv_mem_psram.c"
         "fonts/deck_font_16.c" "fonts/deck_font_20.c" "fonts/deck_font_28.c" "fonts/deck_font_40.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag esp_wifi esp_netif esp_event
             esp_timer esp_https_ota esp_http_client app_update nvs_flash json esp_psram esp_mm fatfs sdmmc mbedtls)
# LVGL 9.6 marca lv_obj_add_flag/remove_flag como obsoletas (continuam a funcionar)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
# alocador do LVGL na PSRAM (lv_mem_psram.c): o LVGL vem depois do main na ligação, por isso força-se a inclusão
foreach(sym lv_mem_init lv_mem_deinit lv_malloc_core lv_realloc_core lv_free_core lv_mem_add_pool lv_mem_remove_pool
            lv_mem_monitor_core lv_mem_test_core)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ${sym}")
endforeach()
