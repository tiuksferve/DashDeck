#pragma once
// Waveshare ESP32-C6-Touch-LCD-1.47: display JD9853 (SPI), toque AXS5106L (I2C), backlight PWM.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 172   // resolução física (retrato)
#define BOARD_LCD_V_RES 320

// Inicializa barramentos, display, toque e LVGL. rotation: 0..3 (0 = retrato, 1/3 = horizontal).
esp_err_t board_init(uint8_t rotation, lv_display_t **out_disp);

// Brilho 0..100 %.
void board_set_backlight(uint8_t percent);

// true enquanto o botão BOOT (GPIO9) estiver pressionado.
bool board_boot_pressed(void);

// Cartão SD (FAT) montado em BOARD_SD_PATH. false se não houver cartão (o resto funciona sem ele).
#define BOARD_SD_PATH "/sd"
bool board_sd_mount(void);
// MB livres no cartão, ou -1 sem cartão.
int board_sd_free_mb(void);
