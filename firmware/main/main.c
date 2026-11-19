// DashDeck — tela auxiliar do uConsole (Waveshare ESP32-C6-Touch-LCD-1.47)
#include "board.h"
#include "esp_log.h"
#include "link.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "dashdeck";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uint8_t rot = 1;   // padrão: horizontal
    nvs_handle_t h;
    if (nvs_open("dash", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "rot", &rot);
        nvs_close(h);
    }
    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_init(rot, &disp));
    ui_init(disp);          // UI pronta antes de começar a receber mensagens
    link_init(ui_handle);
    ESP_LOGI(TAG, "DashDeck %s pronto", DASHDECK_FW_VERSION);
}
