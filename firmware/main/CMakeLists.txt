idf_component_register(
    SRCS "main.c" "board.c" "link.c" "ui.c" "clockface.c" "bootanim.c" "notify.c"
         "fonts/dash_font_12.c" "fonts/dash_font_14.c" "fonts/dash_font_16.c" "fonts/dash_font_20.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc
             esp_driver_usb_serial_jtag esp_lcd json nvs_flash esp_timer
             esp_lcd_jd9853 esp_lcd_touch_axs5106 mbedtls esp_system bootloader_support
             fatfs sdmmc esp_driver_sdspi)
