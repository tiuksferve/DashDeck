#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RX_LINE_MAX     8192
#define HOST_TIMEOUT_US (6 * 1000 * 1000)

static SemaphoreHandle_t s_tx_lock;
static link_msg_cb_t s_on_msg;
static volatile int64_t s_last_rx_us;
static volatile bool s_ready;

static void tx_raw(const char *data, size_t len)
{
    if (!s_ready || len == 0) {
        return;
    }
    // Timeout curto: se ninguém lê a porta, descartamos em vez de travar.
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(20));
}

void link_send(const cJSON *msg)
{
    if (!s_ready) {
        return;   // ainda sem driver/mutex (ex.: chamado durante ui_init)
    }
    char *s = cJSON_PrintUnformatted(msg);
    if (!s) {
        return;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    tx_raw(s, strlen(s));
    tx_raw("\n", 1);
    xSemaphoreGive(s_tx_lock);
    cJSON_free(s);
}

void link_send_event(const char *type, const char *k1, const char *v1, const char *k2, const char *v2)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", type);
    if (k1 && v1) {
        cJSON_AddStringToObject(o, k1, v1);
    }
    if (k2 && v2) {
        cJSON_AddStringToObject(o, k2, v2);
    }
    link_send(o);
    cJSON_Delete(o);
}

bool link_host_alive(void)
{
    return s_last_rx_us && (esp_timer_get_time() - s_last_rx_us) < HOST_TIMEOUT_US;
}

// Logs do ESP-IDF -> "# ..." pela USB
static int log_vprintf(const char *fmt, va_list ap)
{
    static char buf[256];
    if (!s_ready || xPortInIsrContext()) {
        return 0;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    buf[0] = '#';
    buf[1] = ' ';
    int n = vsnprintf(buf + 2, sizeof(buf) - 3, fmt, ap);
    if (n > 0) {
        size_t len = strnlen(buf, sizeof(buf) - 1);
        // Garante exatamente um '\n' no fim, sem '\n' internos
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                buf[i] = ' ';
            }
        }
        while (len > 2 && buf[len - 1] == ' ') {
            len--;
        }
        buf[len++] = '\n';
        tx_raw(buf, len);
    }
    xSemaphoreGive(s_tx_lock);
    return n;
}

static void rx_task(void *arg)
{
    static char line[RX_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    static uint8_t chunk[512];

    while (true) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n') {
                if (!overflow && len > 0) {
                    line[len] = '\0';
                    s_last_rx_us = esp_timer_get_time();
                    cJSON *msg = cJSON_ParseWithLength(line, len);
                    if (msg) {
                        if (s_on_msg) {
                            s_on_msg(msg);
                        }
                        cJSON_Delete(msg);
                    } else {
                        ESP_LOGW("link", "JSON inválido (%u bytes)", (unsigned)len);
                    }
                } else if (overflow) {
                    ESP_LOGW("link", "linha excedeu %d bytes, descartada", RX_LINE_MAX);
                }
                len = 0;
                overflow = false;
            } else if (c != '\r') {
                if (len < RX_LINE_MAX - 1) {
                    line[len++] = c;
                } else {
                    overflow = true;
                }
            }
        }
    }
}

void link_init(link_msg_cb_t on_msg)
{
    s_on_msg = on_msg;
    s_tx_lock = xSemaphoreCreateMutex();
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 16384;   // folga enquanto a UI desenha (antes perdia bytes)
    cfg.tx_buffer_size = 2048;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    s_ready = true;
    esp_log_set_vprintf(log_vprintf);
    xTaskCreate(rx_task, "link_rx", 6144, NULL, 5, NULL);
}
