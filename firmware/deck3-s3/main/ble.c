#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/bas/ble_svc_bas.h"
#include "services/dis/ble_svc_dis.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store.h"

static const char *TAG = "ble";

static ble_info_t s_info;
static uint8_t s_own_addr_type;
static int s_last_pct = -1;

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_info.connected = ev->connect.status == 0;
        ESP_LOGI(TAG, "ligação %s", s_info.connected ? "aceite" : "falhou");
        if (!s_info.connected) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_info.connected = false;
        ESP_LOGI(TAG, "desligado (%d)", ev->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_info.on) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_info.name;
    f.name_len = strlen(s_info.name);
    f.name_is_complete = 1;
    ble_uuid16_t bas = BLE_UUID16_INIT(BLE_SVC_BAS_UUID16);
    f.uuids16 = &bas;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) {
        snprintf(s_info.err, sizeof(s_info.err), "adv_set_fields %d", rc);
        return;
    }
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) snprintf(s_info.err, sizeof(s_info.err), "adv_start %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t a[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    snprintf(s_info.addr, sizeof(s_info.addr), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    ESP_LOGI(TAG, "%s em %s", s_info.name, s_info.addr);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "reset do host BLE (%d)", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();   // até nimble_port_stop()
    nimble_port_freertos_deinit();
}

static bool start(void)
{
    s_info.err[0] = '\0';
    // O controlador BLE precisa de RAM interna; sem margem, falhar com uma mensagem em vez de bloquear
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < 48 * 1024) {
        snprintf(s_info.err, sizeof(s_info.err), "pouca RAM interna (%u KB)", (unsigned)(free_int / 1024));
        ESP_LOGE(TAG, "%s", s_info.err);
        return false;
    }
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        snprintf(s_info.err, sizeof(s_info.err), "nimble_port_init: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "%s", s_info.err);
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_bas_init();
    ble_svc_dis_init();
    ble_svc_dis_model_number_set("Deck3");
    ble_svc_gap_device_name_set(s_info.name);
    ble_svc_gap_device_appearance_set(0x00C0);   // relógio genérico (o mais parecido com um cubo de mesa)
    if (s_last_pct >= 0) ble_svc_bas_battery_level_set(s_last_pct);
    s_info.on = true;
    nimble_port_freertos_init(host_task);
    return true;
}

static void stop(void)
{
    s_info.on = false;
    s_info.connected = false;
    if (nimble_port_stop() == 0) nimble_port_deinit();
}

void ble_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_info.name, sizeof(s_info.name), "Deck\xC2\xB3 %02x%02x", mac[4], mac[5]);
    if (store_get("bt", 0)) start();
}

bool ble_set(bool on)
{
    if (on == s_info.on) return true;
    bool ok = true;
    if (on) ok = start();
    else stop();
    store_set("bt", s_info.on);
    return ok;
}

void ble_get(ble_info_t *out)
{
    *out = s_info;
}

void ble_battery(int pct)
{
    if (pct < 0 || pct > 100 || pct == s_last_pct) return;
    s_last_pct = pct;
    if (s_info.on) ble_svc_bas_battery_level_set(pct);
}

static void reply(link_via_t via)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "bt");
    cJSON_AddBoolToObject(o, "on", s_info.on);
    cJSON_AddBoolToObject(o, "conn", s_info.connected);
    cJSON_AddStringToObject(o, "name", s_info.name);
    cJSON_AddStringToObject(o, "addr", s_info.addr);
    if (s_info.err[0]) cJSON_AddStringToObject(o, "err", s_info.err);
    link_send_via(o, via);
    cJSON_Delete(o);
}

bool ble_handle(const cJSON *msg, link_via_t via)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t) return false;
    if (!strcmp(t, "bt")) {
        const cJSON *on = cJSON_GetObjectItem(msg, "on");
        if (cJSON_IsBool(on)) ble_set(cJSON_IsTrue(on));
        reply(via);
        return true;
    }
    if (!strcmp(t, "bt?")) {
        reply(via);
        return true;
    }
    return false;
}
