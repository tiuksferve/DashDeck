#include "audio.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "link.h"
#include "store.h"

static const char *TAG = "audio";

#define RATE   16000
#define CHANS  2
#define FRAMES 256

typedef struct {
    int16_t freq, ms;
    int8_t vol;
} tone_t;

static esp_codec_dev_handle_t s_spk, s_mic;
static QueueHandle_t s_tones;
static volatile int s_mic_hz;
static int s_vol = 60;

void audio_tone(int freq, int ms, int vol)
{
    if (!s_tones) return;
    tone_t t = {.freq = freq, .ms = ms, .vol = vol};
    xQueueSend(s_tones, &t, 0);
}

static void tone_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(FRAMES * CHANS * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    tone_t t;
    float phase = 0;
    while (true) {
        if (xQueueReceive(s_tones, &t, portMAX_DELAY) != pdTRUE) continue;
        esp_codec_dev_set_out_vol(s_spk, t.vol > 0 ? t.vol : s_vol);
        int total = RATE * t.ms / 1000, done = 0, fade = RATE / 200;   // rampas de 5 ms: sem estalos
        while (done < total) {
            int n = total - done > FRAMES ? FRAMES : total - done;
            for (int i = 0; i < n; i++) {
                int k = done + i;
                float env = 1.0f;
                if (k < fade) env = (float)k / fade;
                else if (total - k < fade) env = (float)(total - k) / fade;
                int16_t s = 0;
                if (t.freq > 0) {
                    s = (int16_t)(sinf(phase) * 12000 * env);
                    phase += 2 * (float)M_PI * t.freq / RATE;
                    if (phase > 2 * (float)M_PI) phase -= 2 * (float)M_PI;
                }
                buf[i * 2] = buf[i * 2 + 1] = s;
            }
            esp_codec_dev_write(s_spk, buf, n * CHANS * sizeof(int16_t));
            done += n;
        }
    }
}

static void mic_task(void *arg)
{
    const int n = 1024;
    int16_t *buf = heap_caps_malloc(n * CHANS * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    TickType_t last = 0;
    while (true) {
        int hz = s_mic_hz;
        if (hz <= 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (esp_codec_dev_read(s_mic, buf, n * CHANS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (xTaskGetTickCount() - last < pdMS_TO_TICKS(1000 / hz)) continue;
        last = xTaskGetTickCount();
        double acc = 0;
        int peak = 0;
        for (int i = 0; i < n * CHANS; i++) {
            int v = buf[i];
            acc += (double)v * v;
            if (abs(v) > peak) peak = abs(v);
        }
        float rms = sqrtf(acc / (n * CHANS));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "mic");
        cJSON_AddNumberToObject(o, "db", roundf(20 * log10f(rms / 32768.0f + 1e-6f) * 10) / 10);
        cJSON_AddNumberToObject(o, "peak", roundf(20 * log10f(peak / 32768.0f + 1e-6f) * 10) / 10);
        link_send(o);
        cJSON_Delete(o);
    }
}

void audio_init(void)
{
    s_vol = store_get("vol", 60);
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "altifalante não iniciou");
        return;
    }
    esp_codec_dev_sample_info_t fs = {.sample_rate = RATE, .channel = CHANS, .bits_per_sample = 16};
    esp_codec_dev_open(s_spk, &fs);
    esp_codec_dev_set_out_vol(s_spk, s_vol);
    if (s_mic) {
        esp_codec_dev_open(s_mic, &fs);
        esp_codec_dev_set_in_gain(s_mic, 24.0f);
        xTaskCreatePinnedToCoreWithCaps(mic_task, "mic", 4096, NULL, 3, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);   // stack na PSRAM: não escreve na flash
    }
    s_tones = xQueueCreate(16, sizeof(tone_t));
    xTaskCreatePinnedToCoreWithCaps(tone_task, "tone", 4096, NULL, 4, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);   // stack na PSRAM: não escreve na flash
}

static int num(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

bool audio_handle(const cJSON *msg)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t) return false;
    if (strcmp(t, "beep") == 0) {
        audio_tone(num(msg, "f", 880), num(msg, "ms", 120), num(msg, "vol", 0));
    } else if (strcmp(t, "tones") == 0) {
        const cJSON *seq = cJSON_GetObjectItem(msg, "seq");
        int vol = num(msg, "vol", 0);
        const cJSON *it;
        cJSON_ArrayForEach(it, seq) {
            if (cJSON_GetArraySize(it) >= 2) {
                audio_tone(cJSON_GetArrayItem(it, 0)->valueint, cJSON_GetArrayItem(it, 1)->valueint, vol);
            }
        }
    } else if (strcmp(t, "vol") == 0) {
        s_vol = num(msg, "v", s_vol);
        if (s_vol < 0) s_vol = 0;
        if (s_vol > 100) s_vol = 100;
        if (s_spk) esp_codec_dev_set_out_vol(s_spk, s_vol);
        store_set("vol", s_vol);
    } else if (strcmp(t, "mic") == 0) {
        s_mic_hz = num(msg, "hz", 0);
    } else {
        return false;
    }
    return true;
}

int audio_get_vol(void)
{
    return s_vol;
}
