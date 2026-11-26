#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "deck.h"

static const char *TAG = "net";

#define MAX_NETS  8
#define NVS_NS    "deck3"

#define BIT_GOT_IP   BIT0
#define BIT_WIFI_ERR BIT1
#define BIT_WS_UP    BIT2
#define BIT_WS_DOWN  BIT3
#define BIT_CFG      BIT4   // redes ou host mudaram: recomeçar

typedef struct {
    char ssid[33];
    char pass[65];
} cred_t;

static cred_t s_nets[MAX_NETS];
static int s_nnets;
static char s_token[65];
static char s_host_cfg[64];    // "ip:porta" fixo, ou vazio = automático
static char s_last_host[64];   // último que funcionou
static char s_dev_id[13];

static EventGroupHandle_t s_ev;
static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;   // criado por lock_get() (a ui lê o estado antes do net_init)

static SemaphoreHandle_t lock_get(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    return s_lock;
}
static net_info_t s_info;
static esp_netif_t *s_netif;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_up;
static char *s_rx_buf;          // remontagem de mensagens maiores que o buffer do cliente
static size_t s_rx_cap;

// ------------------------------------------------------------------------------------------ NVS

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_nets);
    if (nvs_get_blob(h, "nets", s_nets, &sz) == ESP_OK) {
        s_nnets = sz / sizeof(cred_t);
    }
    sz = sizeof(s_token);
    nvs_get_str(h, "token", s_token, &sz);
    sz = sizeof(s_host_cfg);
    nvs_get_str(h, "host", s_host_cfg, &sz);
    sz = sizeof(s_last_host);
    nvs_get_str(h, "lasthost", s_last_host, &sz);
    nvs_close(h);
}

static void nvs_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_nets(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_nnets) nvs_set_blob(h, "nets", s_nets, s_nnets * sizeof(cred_t));
    else nvs_erase_key(h, "nets");
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------------------- estado

static void set_state(net_state_t st)
{
    xSemaphoreTake(lock_get(), portMAX_DELAY);
    s_info.state = st;
    s_info.nets = s_nnets;
    xSemaphoreGive(s_lock);
}

void net_get_info(net_info_t *out)
{
    xSemaphoreTake(lock_get(), portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
    wifi_ap_record_t ap;
    if (out->state >= NET_DISCOVERING && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
    } else {
        out->rssi = 0;
    }
}

const char *net_state_text(net_state_t s)
{
    switch (s) {
    case NET_NO_CREDS: return "Sem redes Wi-Fi: ligue pela USB e corra setup/provision.py";
    case NET_SCANNING: return "A procurar redes Wi-Fi...";
    case NET_CONNECTING: return "A ligar ao Wi-Fi...";
    case NET_DISCOVERING: return "A procurar o uConsole...";
    case NET_HOST_CONNECTING: return "A ligar ao uConsole...";
    case NET_ONLINE: return "Ligado";
    }
    return "";
}

const char *net_device_id(void)
{
    return s_dev_id;
}

// --------------------------------------------------------------------------------------- Wi-Fi

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        xEventGroupSetBits(s_ev, BIT_WIFI_ERR);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        xSemaphoreTake(lock_get(), portMAX_DELAY);
        snprintf(s_info.ip, sizeof(s_info.ip), IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_lock);
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

// A rede guardada com melhor sinal entre as que estão ao alcance; -1 se nenhuma.
static int scan_pick(void)
{
    set_state(NET_SCANNING);
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return -1;
    uint16_t n = 24;
    wifi_ap_record_t *aps = heap_caps_calloc(n, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (!aps) return -1;
    esp_wifi_scan_get_ap_records(&n, aps);
    int best = -1, best_rssi = -1000;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < s_nnets; k++) {
            if (strcmp((const char *)aps[i].ssid, s_nets[k].ssid) == 0 && aps[i].rssi > best_rssi) {
                best = k;
                best_rssi = aps[i].rssi;
            }
        }
    }
    heap_caps_free(aps);
    if (best >= 0) ESP_LOGI(TAG, "rede escolhida: %s (%d dBm)", s_nets[best].ssid, best_rssi);
    return best;
}

static bool wifi_connect(int k)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_nets[k].ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_nets[k].pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s_nets[k].pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    xSemaphoreTake(lock_get(), portMAX_DELAY);
    strlcpy(s_info.ssid, s_nets[k].ssid, sizeof(s_info.ssid));
    s_info.ip[0] = '\0';
    xSemaphoreGive(s_lock);
    set_state(NET_CONNECTING);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(s_ev, BIT_WIFI_ERR | BIT_GOT_IP);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_WIFI_ERR, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (b & BIT_GOT_IP) return true;
    esp_wifi_disconnect();
    return false;
}

// ------------------------------------------------------------------------------------- WebSocket

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_up = true;
        xEventGroupSetBits(s_ev, BIT_WS_UP);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        if (s_ws_up) link_transport_down(LINK_WIFI);
        s_ws_up = false;
        xEventGroupSetBits(s_ev, BIT_WS_DOWN);
        break;
    case WEBSOCKET_EVENT_DATA:
        // 0x1 texto (0x0 continuação); 0x2 binário fica para imagens/áudio (ainda não usado)
        if ((d->op_code == 0x1 || d->op_code == 0x0) && d->payload_len > 0) {
            size_t total = d->payload_len;
            if (d->payload_offset == 0 && d->data_len >= (int)total) {
                link_push(d->data_ptr, d->data_len, LINK_WIFI);   // caso normal: coube no buffer
                break;
            }
            if (d->payload_offset == 0 && total > s_rx_cap) {
                heap_caps_free(s_rx_buf);
                s_rx_buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
                s_rx_cap = s_rx_buf ? total : 0;
            }
            if (s_rx_buf && d->payload_offset + d->data_len <= (int)s_rx_cap) {
                memcpy(s_rx_buf + d->payload_offset, d->data_ptr, d->data_len);
                if (d->payload_offset + d->data_len >= (int)total) link_push(s_rx_buf, total, LINK_WIFI);
            }
        }
        break;
    default:
        break;
    }
}

bool net_ws_connected(void)
{
    return s_ws_up;
}

int net_ws_send(const char *data, size_t len)
{
    if (!s_ws || !s_ws_up) return -1;
    int r = esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(2000));
    return r < 0 ? -1 : 0;
}

static void ws_stop(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_ws_up) link_transport_down(LINK_WIFI);
    s_ws_up = false;
}

static bool ws_try(const char *hostport)
{
    char uri[200];
    snprintf(uri, sizeof(uri), "ws://%s/deck?id=%s&token=%s&fw=%s", hostport, s_dev_id, s_token, DECK3_FW_VERSION);
    xSemaphoreTake(lock_get(), portMAX_DELAY);
    strlcpy(s_info.host, hostport, sizeof(s_info.host));
    xSemaphoreGive(s_lock);
    set_state(NET_HOST_CONNECTING);
    ESP_LOGI(TAG, "a ligar a ws://%s", hostport);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = 8192,
        .task_stack = 6144,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 5000,
        .ping_interval_sec = 5,
    };
    xEventGroupClearBits(s_ev, BIT_WS_UP | BIT_WS_DOWN);
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return false;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_WS_UP | BIT_WS_DOWN, pdFALSE, pdFALSE, pdMS_TO_TICKS(7000));
    if (b & BIT_WS_UP) {
        if (strcmp(s_last_host, hostport) != 0) {
            strlcpy(s_last_host, hostport, sizeof(s_last_host));
            nvs_save_str("lasthost", s_last_host);
        }
        return true;
    }
    ws_stop();
    return false;
}

// Lista de candidatos, por ordem. Devolve quantos.
static int discover(char out[][64], int max)
{
    int n = 0;
    if (s_host_cfg[0]) {
        strlcpy(out[n++], s_host_cfg, 64);
        return n;
    }
    mdns_result_t *res = NULL;
    if (mdns_query_ptr("_deck3", "_tcp", 2500, 4, &res) == ESP_OK) {
        for (mdns_result_t *r = res; r && n < max; r = r->next) {
            for (mdns_ip_addr_t *a = r->addr; a && n < max; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(out[n++], 64, IPSTR ":%u", IP2STR(&a->addr.u_addr.ip4), r->port);
                }
            }
        }
        mdns_query_results_free(res);
    }
    if (s_last_host[0] && n < max) strlcpy(out[n++], s_last_host, 64);
    esp_netif_ip_info_t ip;
    if (n < max && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.gw.addr) {
        snprintf(out[n++], 64, IPSTR ":%d", IP2STR(&ip.gw), DECK3_WS_PORT);
    }
    return n;
}

// ------------------------------------------------------------------------------------ tarefa

static void net_task(void *arg)
{
    bool sntp_started = false;
    while (true) {
        xEventGroupClearBits(s_ev, BIT_CFG);
        if (s_nnets == 0) {
            set_state(NET_NO_CREDS);
            xEventGroupWaitBits(s_ev, BIT_CFG, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        if (!(xEventGroupGetBits(s_ev) & BIT_GOT_IP)) {
            int k = scan_pick();
            if (k < 0 || !wifi_connect(k)) {
                xEventGroupWaitBits(s_ev, BIT_CFG, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
                continue;
            }
            if (!sntp_started) {   // hora certa mesmo sem o deckd
                esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                esp_netif_sntp_init(&sc);
                sntp_started = true;
            }
        }
        set_state(NET_DISCOVERING);
        char cands[6][64];
        int n = discover(cands, 6);
        bool up = false;
        for (int i = 0; i < n && !up; i++) {
            up = ws_try(cands[i]);
        }
        if (!up) {
            set_state(NET_DISCOVERING);
            xEventGroupWaitBits(s_ev, BIT_CFG, pdTRUE, pdFALSE, pdMS_TO_TICKS(4000));
            continue;
        }
        set_state(NET_ONLINE);
        // Rollback: um firmware novo só fica "válido" depois de chegar ao deckd. Se reiniciar antes
        // (crash, sem memória...), o bootloader volta à versão anterior sozinho.
        esp_ota_mark_app_valid_cancel_rollback();
        link_transport_up(LINK_WIFI);
        // espera até cair o WebSocket, o Wi-Fi ou a configuração mudar
        while (true) {
            EventBits_t b = xEventGroupWaitBits(s_ev, BIT_WS_DOWN | BIT_WIFI_ERR | BIT_CFG, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(1000));
            if (b & (BIT_WS_DOWN | BIT_WIFI_ERR | BIT_CFG)) break;
        }
        ESP_LOGW(TAG, "ligação ao deckd perdida");
        ws_stop();
        xEventGroupClearBits(s_ev, BIT_WIFI_ERR);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------------- configuração

static void reply_cfg(link_via_t via, bool ok, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "cfg");
    cJSON_AddBoolToObject(o, "ok", ok);
    if (err) cJSON_AddStringToObject(o, "err", err);
    cJSON *arr = cJSON_AddArrayToObject(o, "nets");
    for (int i = 0; i < s_nnets; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(s_nets[i].ssid));
    net_info_t inf;
    net_get_info(&inf);
    cJSON_AddStringToObject(o, "state", net_state_text(inf.state));
    cJSON_AddStringToObject(o, "ssid", inf.ssid);
    cJSON_AddStringToObject(o, "ip", inf.ip);
    cJSON_AddStringToObject(o, "host", s_host_cfg[0] ? s_host_cfg : "auto");
    cJSON_AddBoolToObject(o, "token", s_token[0] != '\0');
    cJSON_AddStringToObject(o, "id", s_dev_id);
    link_send_via(o, via);
    cJSON_Delete(o);
}

// Procura de redes a pedido ("wifi" {"scan":true}): responde "wifi_scan" com as redes ao alcance.
// Só com o Wi-Fi já ligado (sem ligação é a tarefa net que anda a procurar).
static void scan_task(void *arg)
{
    link_via_t via = (link_via_t)(intptr_t)arg;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "wifi_scan");
    uint16_t n = 24;
    wifi_ap_record_t *aps = heap_caps_calloc(n, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    esp_err_t e = aps ? esp_wifi_scan_start(NULL, true) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = esp_wifi_scan_get_ap_records(&n, aps);
    if (e != ESP_OK) {
        cJSON_AddStringToObject(o, "err", esp_err_to_name(e));
    } else {
        cJSON *arr = cJSON_AddArrayToObject(o, "aps");
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)aps[i].ssid;
            if (!ssid[0]) continue;
            bool dup = false;   // a mesma rede em vários APs: fica a primeira (a de melhor sinal)
            for (int j = 0; j < i; j++) dup |= strcmp(ssid, (const char *)aps[j].ssid) == 0;
            if (dup) continue;
            bool saved = false;
            for (int k = 0; k < s_nnets; k++) saved |= strcmp(ssid, s_nets[k].ssid) == 0;
            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "ssid", ssid);
            cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
            cJSON_AddNumberToObject(a, "ch", aps[i].primary);
            cJSON_AddBoolToObject(a, "lock", aps[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddBoolToObject(a, "saved", saved);
            cJSON_AddItemToArray(arr, a);
        }
    }
    heap_caps_free(aps);
    link_send_via(o, via);
    cJSON_Delete(o);
    vTaskDelete(NULL);
}

bool net_handle_cfg(const cJSON *msg, link_via_t via)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t) return false;
    if (strcmp(t, "cfg?") == 0 || strcmp(t, "wifi?") == 0) {
        reply_cfg(via, true, NULL);
        return true;
    }
    if (strcmp(t, "wifi") == 0) {
        // {"ssid","pass"} acrescenta/atualiza; {"del":"ssid"} apaga; {"clear":true} apaga todas.
        // Também aceite do deckd pela rede: o uConsole pode partilhar as redes que conhece.
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "ssid"));
        const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "pass"));
        const char *del = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "del"));
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "scan"))) {
            if (s_info.state < NET_DISCOVERING) {
                reply_cfg(via, false, "Wi-Fi ainda a ligar");
            } else {
                xTaskCreate(scan_task, "wscan", 4096, (void *)(intptr_t)via, 3, NULL);
            }
            return true;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "clear"))) {
            s_nnets = 0;
        } else if (del) {
            for (int i = 0; i < s_nnets; i++) {
                if (strcmp(s_nets[i].ssid, del) == 0) {
                    memmove(&s_nets[i], &s_nets[i + 1], (s_nnets - i - 1) * sizeof(cred_t));
                    s_nnets--;
                    break;
                }
            }
        } else if (ssid && *ssid && strlen(ssid) < 33 && (!pass || strlen(pass) < 65)) {
            int k = -1;
            for (int i = 0; i < s_nnets; i++) {
                if (strcmp(s_nets[i].ssid, ssid) == 0) k = i;
            }
            if (k < 0) {
                if (s_nnets == MAX_NETS) {   // cheio: sai a mais antiga
                    memmove(&s_nets[0], &s_nets[1], (MAX_NETS - 1) * sizeof(cred_t));
                    s_nnets--;
                }
                k = s_nnets++;
            }
            strlcpy(s_nets[k].ssid, ssid, sizeof(s_nets[k].ssid));
            strlcpy(s_nets[k].pass, pass ? pass : "", sizeof(s_nets[k].pass));
        } else {
            reply_cfg(via, false, "ssid/pass inválidos");
            return true;
        }
        nvs_save_nets();
        reply_cfg(via, true, NULL);
        // pela rede, não derruba a ligação atual só por acrescentar uma rede
        if (via == LINK_USB || s_info.state != NET_ONLINE) xEventGroupSetBits(s_ev, BIT_CFG);
        return true;
    }
    if (strcmp(t, "token") == 0 || strcmp(t, "host") == 0) {
        if (via != LINK_USB) {
            reply_cfg(via, false, "só pela USB");
            return true;
        }
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(msg, strcmp(t, "token") == 0 ? "v" : "addr"));
        if (!v) v = "";
        if (strcmp(t, "token") == 0) {
            strlcpy(s_token, v, sizeof(s_token));
            nvs_save_str("token", s_token);
        } else {
            strlcpy(s_host_cfg, v, sizeof(s_host_cfg));
            nvs_save_str("host", s_host_cfg);
        }
        reply_cfg(via, true, NULL);
        xEventGroupSetBits(s_ev, BIT_CFG);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------ init

void net_init(void)
{
    s_ev = xEventGroupCreate();
    lock_get();
    nvs_load();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_id, sizeof(s_dev_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    char hn[16];
    snprintf(hn, sizeof(hn), "deck3-%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(s_netif, hn);
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // as redes vivem na nossa NVS, não na do driver
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);       // poupança com latência aceitável
    ESP_ERROR_CHECK(esp_wifi_start());
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(hn);
    }
    xTaskCreate(net_task, "net", 6144, NULL, 4, NULL);
}
