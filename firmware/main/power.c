#include "power.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sd.h"
#include "store.h"
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "keys.h"

static const char *TAG = "power";

#define AXP_ADDR      0x34
#define REG_STATUS1   0x00   // bit5 VBUS bom, bit3 bateria ligada
#define REG_STATUS2   0x01   // bits 7..5: 001 = a carregar
#define REG_ADC_CTRL  0x30   // bit0 medir a tensão da bateria
#define REG_VBAT_H    0x34   // tensão: H[4:0] L[7:0] em mV
#define REG_VBUS_H    0x38
#define REG_VSYS_H    0x3A
#define REG_TDIE_H    0x3C
#define REG_ICC       0x62   // corrente de carga
#define REG_CV        0x64   // tensão final de carga
#define REG_INTEN2    0x41   // bit3 toque curto no PWR, bit2 toque longo
#define REG_INTSTS1   0x48
#define REG_INTSTS2   0x49
#define REG_INTSTS3   0x4A
#define REG_BAT_DET   0x68   // bit0 deteção da bateria
#define REG_BAT_PCT   0xA4

static i2c_master_dev_handle_t s_dev;
static power_info_t s_info;

#define HIST_N 720            // 12 h a 1 amostra/min
typedef struct {
    uint32_t t;               // hora UTC (epoch): o histórico sobrevive a reinícios (fica no cartão)
    uint16_t mv;
    int8_t pct;
    uint8_t flags;            // bit0 a carregar, bit1 USB
} sample_t;
static sample_t *s_hist;
static int s_hist_n, s_hist_w;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static void set_bit(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    if (rd(reg, &v, 1) == ESP_OK) wr(reg, v | mask);
}

static int rd14(uint8_t reg)
{
    uint8_t v[2];
    return rd(reg, v, 2) == ESP_OK ? ((v[0] & 0x3F) << 8) | v[1] : 0;
}

static void read_battery(void)
{
    uint8_t s1, s2, pct, v[2];
    if (rd(REG_STATUS1, &s1, 1) != ESP_OK || rd(REG_STATUS2, &s2, 1) != ESP_OK) {
        s_info.ok = false;
        return;
    }
    s_info.ok = true;
    s_info.usb = s1 & (1 << 5);
    s_info.battery = s1 & (1 << 3);
    s_info.charging = ((s2 >> 5) & 0x03) == 0x01;
    s_info.stage = s2 & 0x07;
    s_info.percent = -1;
    s_info.mv = 0;
    if (s_info.battery) {
        if (rd(REG_BAT_PCT, &pct, 1) == ESP_OK) s_info.percent = pct > 100 ? 100 : pct;
        if (rd(REG_VBAT_H, v, 2) == ESP_OK) s_info.mv = ((v[0] & 0x1F) << 8) | v[1];
    }
    s_info.vbus_mv = s_info.usb ? rd14(REG_VBUS_H) : 0;
    s_info.vsys_mv = rd14(REG_VSYS_H);
    int raw = rd14(REG_TDIE_H);
    s_info.temp = raw ? 22.0f + (7274 - raw) / 20.0f : 0;
    uint8_t icc, cv;
    if (rd(REG_ICC, &icc, 1) == ESP_OK) {
        icc &= 0x1F;
        s_info.icc_ma = icc <= 8 ? icc * 25 : 200 + (icc - 8) * 100;
    }
    static const int cvs[] = {0, 4000, 4100, 4200, 4350, 4400};
    if (rd(REG_CV, &cv, 1) == ESP_OK && (cv & 0x07) < 6) s_info.cv_mv = cvs[cv & 0x07];
}

static bool set_icc(int ma);

// Histórico no cartão (deck3/log/bat.bin, registos de 8 bytes): no arranque recupera as últimas 12 h
#define BAT_LOG "log/bat.bin"
static void hist_load(void)
{
    char p[160];
    if (!s_hist || !sd_ok() || !sd_path(BAT_LOG, p, sizeof(p))) return;
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f), n = sz / (long)sizeof(sample_t);
    long first = n > HIST_N ? n - HIST_N : 0;
    fseek(f, first * (long)sizeof(sample_t), SEEK_SET);
    uint32_t cutoff = 0;
    sample_t e;
    while (fread(&e, sizeof(e), 1, f) == 1) {
        if (e.t < 1700000000 || e.t < cutoff) continue;
        cutoff = e.t;
        s_hist[s_hist_w] = e;
        s_hist_w = (s_hist_w + 1) % HIST_N;
        if (s_hist_n < HIST_N) s_hist_n++;
    }
    fclose(f);
    ESP_LOGI(TAG, "histórico da bateria: %d amostras recuperadas do cartão", s_hist_n);
}

static void hist_add(void)
{
    time_t now = time(NULL);
    if (!s_hist || s_info.percent < 0 || now < 1700000000) return;   // sem hora certa ainda (SNTP/deckd)
    s_hist[s_hist_w] = (sample_t){
        .t = (uint32_t)now,
        .mv = s_info.mv,
        .pct = s_info.percent,
        .flags = (s_info.charging ? 1 : 0) | (s_info.usb ? 2 : 0),
    };
    sd_append(BAT_LOG, &s_hist[s_hist_w], sizeof(sample_t));   // 8 bytes por minuto (~11 KB/dia)
    s_hist_w = (s_hist_w + 1) % HIST_N;
    if (s_hist_n < HIST_N) s_hist_n++;
}

static void power_task(void *arg)
{
    int tick = 0;
    while (true) {
        uint8_t st2;
        // A linha IRQ do AXP não chega a um GPIO: lê-se o registo a cada 100 ms.
        if (rd(REG_INTSTS2, &st2, 1) == ESP_OK && (st2 & 0x0C)) {
            wr(REG_INTSTS2, st2 & 0x0C);   // limpa escrevendo 1
            if (st2 & 0x08) keys_emit(KEY_PWR, KEY_EV_SHORT);
            if (st2 & 0x04) keys_emit(KEY_PWR, KEY_EV_LONG);
        }
        if (tick % 50 == 0) read_battery();     // a cada 5 s
        if (tick % 600 == 0) hist_add();        // a cada minuto
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void power_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 sem I2C");
        return;
    }
    uint8_t v;
    if (rd(REG_STATUS1, &v, 1) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 não responde em 0x%02x", AXP_ADDR);
        return;
    }
    set_bit(REG_BAT_DET, 0x01);
    set_bit(REG_ADC_CTRL, 0x1D);   // medir VBAT, VBUS, VSYS e temperatura (só medição)
    s_hist = heap_caps_calloc(HIST_N, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    hist_load();
    set_bit(REG_INTEN2, 0x0C);
    wr(REG_INTSTS1, 0xFF);
    wr(REG_INTSTS2, 0xFF);
    wr(REG_INTSTS3, 0xFF);
    int icc = store_get("icc", 0);
    if (icc) set_icc(icc);
    read_battery();
    ESP_LOGI(TAG, "AXP2101: bateria %d%% %dmV%s%s", s_info.percent, s_info.mv, s_info.charging ? " a carregar" : "",
             s_info.usb ? " USB" : "");
    xTaskCreatePinnedToCoreWithCaps(power_task, "power", 3072, NULL, 3, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);   // stack na PSRAM: não escreve na flash
}

void power_get(power_info_t *out)
{
    *out = s_info;
}

// Corrente de carga (REG 0x62[4:0]): 0..8 = 0..200 mA (25 mA/passo), 9..16 = 300..1000 mA (100 mA/passo).
// De fábrica vem a 25 mA (lentíssimo para 1000 mAh). Só se muda por escolha do utilizador (box Config),
// limitada a 100..500 mA (≤ 0,5C da bateria de 1000 mAh); fica na NVS e é reaplicada no arranque.
static bool set_icc(int ma)
{
    if (ma < 100 || ma > 500) return false;
    uint8_t code = ma <= 200 ? ma / 25 : 8 + (ma - 200) / 100;
    uint8_t v;
    if (rd(REG_ICC, &v, 1) != ESP_OK) return false;
    if (wr(REG_ICC, (v & 0xE0) | code) != ESP_OK) return false;
    ESP_LOGI(TAG, "corrente de carga: %d mA", ma);
    return true;
}

static const char *stage_text(const power_info_t *p)
{
    if (!p->battery) return "sem bateria";
    if (!p->usb) return "a descarregar";
    switch (p->stage) {
    case 0: return "carga lenta";
    case 1: return "pré-carga";
    case 2: return "a carregar (corrente constante)";
    case 3: return "a carregar (tensão constante)";
    case 4: return "carregada";
    default: return "USB, sem carregar";
    }
}

bool power_handle(const cJSON *msg, link_via_t via)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t) return false;
    if (!strcmp(t, "bat_cfg")) {          // {"t":"bat_cfg","icc":400}
        const cJSON *icc = cJSON_GetObjectItem(msg, "icc");
        if (cJSON_IsNumber(icc) && set_icc(icc->valueint)) store_set("icc", icc->valueint);
        t = "bat?";                         // responde com o estado novo
    }
    if (strcmp(t, "bat?") != 0) return false;
    read_battery();
    power_info_t p = s_info;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "bat");
    cJSON_AddNumberToObject(o, "pct", p.percent);
    cJSON_AddNumberToObject(o, "mv", p.mv);
    cJSON_AddBoolToObject(o, "chg", p.charging);
    cJSON_AddBoolToObject(o, "usb", p.usb);
    cJSON_AddBoolToObject(o, "bat", p.battery);
    cJSON_AddNumberToObject(o, "stage", p.stage);
    cJSON_AddStringToObject(o, "state", stage_text(&p));
    cJSON_AddNumberToObject(o, "vbus", p.vbus_mv);
    cJSON_AddNumberToObject(o, "vsys", p.vsys_mv);
    cJSON_AddNumberToObject(o, "temp", (int)(p.temp * 10) / 10.0);
    cJSON_AddNumberToObject(o, "icc", p.icc_ma);
    cJSON_AddNumberToObject(o, "cv", p.cv_mv);
    cJSON_AddNumberToObject(o, "up", (double)(esp_timer_get_time() / 1000000));
    uint32_t now = (uint32_t)time(NULL);
    // histórico: [idade em s, %, mV, flags], do mais antigo para o mais recente, reduzido a n pontos
    int want = 120;
    const cJSON *n = cJSON_GetObjectItem(msg, "n");
    if (cJSON_IsNumber(n) && n->valueint > 1 && n->valueint <= HIST_N) want = n->valueint;
    cJSON *h = cJSON_AddArrayToObject(o, "hist");
    int cnt = s_hist_n, step = cnt > want ? (cnt + want - 1) / want : 1;
    for (int i = 0; i < cnt; i += step) {
        const sample_t *e = &s_hist[(s_hist_w - cnt + i + HIST_N) % HIST_N];
        cJSON *r = cJSON_CreateArray();
        cJSON_AddItemToArray(r, cJSON_CreateNumber(now > e->t ? now - e->t : 0));
        cJSON_AddItemToArray(r, cJSON_CreateNumber(e->pct));
        cJSON_AddItemToArray(r, cJSON_CreateNumber(e->mv));
        cJSON_AddItemToArray(r, cJSON_CreateNumber(e->flags));
        cJSON_AddItemToArray(h, r);
    }
    link_send_via(o, via);
    cJSON_Delete(o);
    return true;
}
