#include "ota.h"

#include <string.h>

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "ui.h"

static const char *TAG = "ota";
static volatile bool s_busy;
static char s_url[200];

static void report(int pct, const char *err, bool ok)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ota");
    if (pct >= 0) cJSON_AddNumberToObject(o, "pct", pct);
    if (err) cJSON_AddStringToObject(o, "err", err);
    if (ok) cJSON_AddBoolToObject(o, "ok", true);
    link_send(o);
    cJSON_Delete(o);
}

static void ota_task(void *arg)
{
    esp_http_client_config_t http = {.url = s_url, .timeout_ms = 10000, .keep_alive_enable = true};
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        report(-1, esp_err_to_name(err), false);
        ui_toast("Atualização falhou", "err");
        s_busy = false;
        vTaskDelete(NULL);
    }
    int total = esp_https_ota_get_image_size(h), last = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int pct = total > 0 ? esp_https_ota_get_image_len_read(h) * 100 / total : 0;
        if (pct / 5 != last / 5) {
            last = pct;
            report(pct, NULL, false);
            ui_progress("A atualizar", pct);
        }
    }
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h) && esp_https_ota_finish(h) == ESP_OK) {
        report(100, NULL, true);
        ui_progress("A reiniciar", 100);
        ESP_LOGI(TAG, "firmware novo gravado, a reiniciar");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    esp_https_ota_abort(h);
    report(-1, esp_err_to_name(err), false);
    ui_progress(NULL, -1);
    ui_toast("Atualização falhou", "err");
    s_busy = false;
    vTaskDelete(NULL);
}

bool ota_handle(const cJSON *msg)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t || strcmp(t, "ota") != 0) return false;
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "url"));
    if (!url || s_busy) {
        report(-1, s_busy ? "ocupado" : "sem url", false);
        return true;
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_busy = true;
    // a stack tem de ser interna (a tarefa escreve na flash); com a RAM interna apertada pode não haver 8 KB
    // seguidos: avisa em vez de ficar calado
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL) != pdPASS) {
        s_busy = false;
        char e[64];
        snprintf(e, sizeof(e), "sem RAM interna para o OTA (maior bloco %u B)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        report(-1, e, false);
    }
    return true;
}
