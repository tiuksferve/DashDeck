#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "net.h"
#include "sd.h"

static const char *TAG = "link";

#define USB_LINE_MAX    (64 * 1024)   // na PSRAM: mensagens grandes (imagens, modelos) cabem
#define HOST_TIMEOUT_US (6 * 1000 * 1000)

typedef struct {
    char *buf;
    size_t len;
    link_via_t via;
} item_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_usb_lock;
static link_msg_cb_t s_on_msg;
static link_hello_cb_t s_on_hello;
static volatile link_via_t s_active = LINK_NONE;
static volatile int64_t s_last_rx_us;
static volatile bool s_usb_ready;

const char *link_via_name(link_via_t via)
{
    return via == LINK_WIFI ? "wifi" : via == LINK_USB ? "usb" : "none";
}

link_via_t link_active(void)
{
    return s_active;
}

bool link_host_alive(void)
{
    return s_last_rx_us && (esp_timer_get_time() - s_last_rx_us) < HOST_TIMEOUT_US;
}

// ------------------------------------------------------------------------------------------- envio

static void usb_tx(const char *data, size_t len)
{
    if (s_usb_ready && len) {
        usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(20));   // sem leitor: descarta, não trava
    }
}

static void send_text(const char *s, link_via_t via)
{
    size_t len = strlen(s);
    if (via == LINK_WIFI) {
        if (net_ws_send(s, len) == 0) {
            return;
        }
        via = LINK_USB;   // o WebSocket caiu entretanto: tenta a USB
    }
    if (via == LINK_USB && s_usb_ready) {
        xSemaphoreTake(s_usb_lock, portMAX_DELAY);
        usb_tx(s, len);
        usb_tx("\n", 1);
        xSemaphoreGive(s_usb_lock);
    }
}

void link_send_via(const cJSON *msg, link_via_t via)
{
    char *s = cJSON_PrintUnformatted(msg);
    if (s) {
        send_text(s, via);
        cJSON_free(s);
    }
}

// Eventos do aparelho que valem a pena guardar quando não há ninguém a ouvir (modo "fora de casa")
static bool keep_offline(const cJSON *msg)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t) return false;
    if (!strcmp(t, "key") || !strcmp(t, "bat_low")) return true;
    if (!strcmp(t, "imu")) return cJSON_GetObjectItem(msg, "face") || cJSON_GetObjectItem(msg, "ev");   // sem o stream
    return false;
}

void link_send(const cJSON *msg)
{
    if (!link_host_alive() && !net_ws_connected() && keep_offline(msg)) {
        sd_event_store(msg);
        return;
    }
    link_via_t via = s_active;
    if (via == LINK_NONE) {
        via = net_ws_connected() ? LINK_WIFI : LINK_USB;
    }
    link_send_via(msg, via);
}

void link_send_text(const char *json)
{
    link_via_t via = s_active;
    if (via == LINK_NONE) via = net_ws_connected() ? LINK_WIFI : LINK_USB;
    send_text(json, via);
}

void link_send_event(const char *type, const char *k1, const char *v1, const char *k2, const char *v2)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", type);
    if (k1 && v1) cJSON_AddStringToObject(o, k1, v1);
    if (k2 && v2) cJSON_AddStringToObject(o, k2, v2);
    link_send(o);
    cJSON_Delete(o);
}

// -------------------------------------------------------------------------------------- receção

void link_push(const char *data, size_t len, link_via_t via)
{
    if (!s_q || !len) return;
    item_t it = {.buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM), .len = len, .via = via};
    if (!it.buf) {
        ESP_LOGW(TAG, "sem memória para %u bytes", (unsigned)len);
        return;
    }
    memcpy(it.buf, data, len);
    it.buf[len] = '\0';
    if (xQueueSend(s_q, &it, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "fila cheia, mensagem descartada");
        heap_caps_free(it.buf);
    }
}

void link_transport_up(link_via_t via)
{
    s_active = via;
    if (s_on_hello) s_on_hello(via);
}

void link_transport_down(link_via_t via)
{
    if (s_active == via) {
        s_active = LINK_NONE;
        s_last_rx_us = 0;
    }
}

static void dispatch_task(void *arg)
{
    item_t it;
    while (true) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;
        cJSON *msg = cJSON_ParseWithLength(it.buf, it.len);
        if (msg) {
            s_last_rx_us = esp_timer_get_time();
            s_active = it.via;
            if (s_on_msg) s_on_msg(msg, it.via);
            cJSON_Delete(msg);
        } else {
            ESP_LOGW(TAG, "JSON inválido (%u bytes, %s)", (unsigned)it.len, link_via_name(it.via));
        }
        heap_caps_free(it.buf);
    }
}

// Logs do ESP-IDF -> "# ..." pela USB (o host ignora o que não for JSON)
static int log_vprintf(const char *fmt, va_list ap)
{
    static char buf[256];
    if (!s_usb_ready || xPortInIsrContext()) return 0;
    if (xSemaphoreTake(s_usb_lock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    buf[0] = '#';
    buf[1] = ' ';
    int n = vsnprintf(buf + 2, sizeof(buf) - 3, fmt, ap);
    if (n > 0) {
        size_t len = strnlen(buf, sizeof(buf) - 1);
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
        }
        while (len > 2 && buf[len - 1] == ' ') len--;
        buf[len++] = '\n';
        usb_tx(buf, len);
    }
    xSemaphoreGive(s_usb_lock);
    return n;
}

static void usb_rx_task(void *arg)
{
    char *line = heap_caps_malloc(USB_LINE_MAX, MALLOC_CAP_SPIRAM);
    static uint8_t chunk[512];
    size_t len = 0;
    bool overflow = false;
    int64_t last_hello = 0;
    while (true) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n') {
                if (!overflow && len) {
                    link_push(line, len, LINK_USB);
                } else if (overflow) {
                    ESP_LOGW(TAG, "linha USB com mais de %d bytes, descartada", USB_LINE_MAX);
                }
                len = 0;
                overflow = false;
            } else if (c != '\r') {
                if (len < USB_LINE_MAX - 1) line[len++] = c;
                else overflow = true;
            }
        }
        // Sem host em nenhum transporte: anuncia-se pela USB a cada 3 s (como o DashDeck)
        int64_t now = esp_timer_get_time();
        if (!link_host_alive() && !net_ws_connected() && now - last_hello > 3000000) {
            last_hello = now;
            if (s_on_hello) s_on_hello(LINK_USB);
        }
    }
}

void link_init(link_msg_cb_t on_msg, link_hello_cb_t on_hello)
{
    s_on_msg = on_msg;
    s_on_hello = on_hello;
    s_q = xQueueCreate(24, sizeof(item_t));
    s_usb_lock = xSemaphoreCreateMutex();
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 16384;
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        s_usb_ready = true;
        esp_log_set_vprintf(log_vprintf);
    }
    xTaskCreate(dispatch_task, "link_disp", 8192, NULL, 5, NULL);
    xTaskCreatePinnedToCoreWithCaps(usb_rx_task, "link_usb", 4096, NULL, 5, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);   // stack na PSRAM: não escreve na flash
}
