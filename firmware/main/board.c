#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_axs5106.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board";

// Pinos (wiki Waveshare + BSP oficial)
#define PIN_SPI_SCLK   GPIO_NUM_1
#define PIN_SPI_MOSI   GPIO_NUM_2
#define PIN_SPI_MISO   GPIO_NUM_3   // usado pelo SD
#define PIN_SD_CS      GPIO_NUM_4
#define PIN_LCD_CS     GPIO_NUM_14
#define PIN_LCD_DC     GPIO_NUM_15
#define PIN_LCD_RST    GPIO_NUM_22
#define PIN_LCD_BL     GPIO_NUM_23
#define PIN_I2C_SDA    GPIO_NUM_18
#define PIN_I2C_SCL    GPIO_NUM_19
#define PIN_TP_RST     GPIO_NUM_20
#define PIN_TP_INT     GPIO_NUM_21
#define PIN_BOOT_BTN   GPIO_NUM_9

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_PCLK_HZ    (80 * 1000 * 1000)
#define LCD_X_GAP      34           // o painel de 172 px fica no meio da RAM de 240 px
#define DRAW_BUF_LINES 40

#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MODE    LEDC_LOW_SPEED_MODE

static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_MODE,
        .timer_num = BL_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t ch = {
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_LCD_BL,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void board_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (percent * 1023U) / 100U;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

bool board_boot_pressed(void)
{
    return gpio_get_level(PIN_BOOT_BTN) == 0;
}

esp_err_t board_init(uint8_t rotation, lv_display_t **out_disp)
{
    // Botão BOOT como entrada (tem pull-up externo, reforçamos o interno)
    const gpio_config_t btn = {
        .pin_bit_mask = BIT64(PIN_BOOT_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    // O SD partilha o SPI: mantém o CS dele inativo (alto)
    const gpio_config_t sdcs = {
        .pin_bit_mask = BIT64(PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&sdcs);
    gpio_set_level(PIN_SD_CS, 1);

    // SPI
    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SPI_SCLK,
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi");

    // Painel
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = JD9853_PANEL_IO_SPI_CONFIG(PIN_LCD_CS, PIN_LCD_DC, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9853(io, &panel_cfg, &panel), TAG, "panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, LCD_X_GAP, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // I2C + toque
    i2c_master_bus_handle_t i2c_bus = NULL;
    const i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c");

    i2c_master_dev_handle_t tp_dev = NULL;
    const i2c_device_config_t tp_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESP_LCD_TOUCH_IO_I2C_AXS5106_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &tp_dev_cfg, &tp_dev), TAG, "tp dev");

    esp_lcd_touch_handle_t touch = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 0},  // retrato (rotação 0)
    };
    esp_err_t tp_err = esp_lcd_touch_new_i2c_axs5106(tp_dev, &tp_cfg, &touch);
    if (tp_err != ESP_OK) {
        ESP_LOGE(TAG, "toque indisponível (%s)", esp_err_to_name(tp_err));
        touch = NULL;
    }

    // LVGL
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 10 * 1024,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add disp");

    if (touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = touch};
        lvgl_port_add_touch(&touch_cfg);
    }

    // Rotação: o LVGL gira o toque; o esp_lvgl_port aplica swap/mirror no painel.
    // O gap (painel de 172 px no meio de 240) muda de eixo quando x/y trocam.
    rotation &= 3;
    lvgl_port_lock(0);
    lv_display_set_rotation(disp, (lv_display_rotation_t)rotation);
    lvgl_port_unlock();
    bool swapped = rotation == 1 || rotation == 3;
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, swapped ? 0 : LCD_X_GAP, swapped ? LCD_X_GAP : 0));

    backlight_init();
    board_set_backlight(80);

    *out_disp = disp;
    return ESP_OK;
}
