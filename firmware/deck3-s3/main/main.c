// Deck³ — painel portátil em cubo para o uConsole, por Wi-Fi (Waveshare ESP32-S3-Touch-AMOLED-2.16)
#include <string.h>

#include "audio.h"
#include "sd.h"
#include "voxel.h"
#include "bsp/esp-bsp.h"
#include "deck.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "imu.h"
#include "keys.h"
#include "link.h"
#include "net.h"
#include "nvs_flash.h"
#include "ota.h"
#include "power.h"
#include "ui.h"

static const char *TAG = "deck3";

static void on_msg(const cJSON *msg, link_via_t via)
{
    if (net_handle_cfg(msg, via)) return;   // wifi, token, host, cfg?
    if (ota_handle(msg)) return;
    if (power_handle(msg, via)) return;     // bat?
    if (sd_handle(msg, via)) {               // sd?, ls, file, rm
        vox_forget_missing();                // um ficheiro novo pode ser um modelo procurado antes
        return;
    }
    ui_handle(msg, via);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lv_display_t *disp = bsp_display_start();   // também inicia o I2C partilhado (toque, PMU, IMU, codecs)
    if (!disp) {
        ESP_LOGE(TAG, "o display não arrancou");
    }
    sd_init();                                  // antes da ui: os modelos de voxels podem vir do cartão
    ui_init(disp);
    link_init(on_msg, ui_send_hello);
    power_init();
    keys_init();
    imu_init();
    audio_init();
    net_init();
    ESP_LOGI(TAG, "Deck³ %s pronto: interna %u livre (maior %u), psram %u", DECK3_FW_VERSION,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
