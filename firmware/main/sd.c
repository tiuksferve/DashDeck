#include "sd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "mbedtls/base64.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";
static bool s_ok;

#define PENDING "log/pendentes.jsonl"
typedef struct {
    char rel[48];
    uint8_t *data;   // NULL = pedido de entrega dos pendentes
    size_t len;
} job_t;
static QueueHandle_t s_jobs;
static volatile int s_pending;

static void flush_now(void);

static void writer_task(void *arg)
{
    job_t j;
    while (xQueueReceive(s_jobs, &j, portMAX_DELAY) == pdTRUE) {
        if (!j.data) {
            flush_now();
            continue;
        }
        char p[160];
        if (sd_path(j.rel, p, sizeof(p))) {
            char d[160];   // pastas intermédias
            strlcpy(d, p, sizeof(d));
            char *slash = strrchr(d, '/');
            if (slash) { *slash = '\0'; mkdir(d, 0775); }
            FILE *f = fopen(p, "ab");
            if (f) {
                fwrite(j.data, 1, j.len, f);
                fclose(f);
            }
        }
        heap_caps_free(j.data);
    }
}

bool sd_append(const char *rel, const void *data, size_t len)
{
    if (!s_ok || !s_jobs || !len) return false;
    job_t j = {.len = len, .data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM)};
    if (!j.data) return false;
    strlcpy(j.rel, rel, sizeof(j.rel));
    memcpy(j.data, data, len);
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        heap_caps_free(j.data);
        return false;
    }
    return true;
}

void sd_event_store(const cJSON *msg)
{
    if (!s_ok) return;
    cJSON *c = cJSON_Duplicate(msg, true);
    time_t now = time(NULL);
    cJSON_AddNumberToObject(c, "ts", now > 1700000000 ? (double)now : 0);
    char *txt = cJSON_PrintUnformatted(c);
    cJSON_Delete(c);
    if (!txt) return;
    size_t n = strlen(txt);
    txt[n] = '\n';   // cJSON reserva o terminador: troca-o pela mudança de linha
    if (sd_append(PENDING, txt, n + 1)) s_pending++;
    cJSON_free(txt);
}

int sd_events_pending(void)
{
    return s_pending;
}

void sd_events_flush(void)
{
    if (!s_ok || !s_jobs || s_pending <= 0) return;
    job_t j = {.data = NULL};
    xQueueSend(s_jobs, &j, 0);
}

// lê os pendentes e envia-os em lotes de 40 (corre na tarefa de escrita: a ordem com as escritas mantém-se)
static void flush_now(void)
{
    char p[160];
    sd_path(PENDING, p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) {
        s_pending = 0;
        return;
    }
    char line[512];
    cJSON *batch = NULL, *arr = NULL;
    int n = 0, total = 0;
    while (fgets(line, sizeof(line), f)) {
        cJSON *e = cJSON_Parse(line);
        if (!e) continue;
        if (!batch) {
            batch = cJSON_CreateObject();
            cJSON_AddStringToObject(batch, "t", "offline_log");
            arr = cJSON_AddArrayToObject(batch, "events");
        }
        cJSON_AddItemToArray(arr, e);
        total++;
        if (++n == 40) {
            link_send(batch);
            cJSON_Delete(batch);
            batch = NULL;
            n = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    fclose(f);
    if (batch) {
        cJSON_AddBoolToObject(batch, "last", true);
        link_send(batch);
        cJSON_Delete(batch);
    }
    unlink(p);
    s_pending = 0;
    ESP_LOGI(TAG, "%d eventos de fora de casa entregues", total);
}

void sd_init(void)
{
    esp_err_t e = bsp_sdcard_mount();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "sem cartão SD (%s)", esp_err_to_name(e));
        return;
    }
    s_ok = true;
    mkdir(SD_ROOT, 0775);
    mkdir(SD_ROOT "/vox", 0775);
    mkdir(SD_ROOT "/log", 0775);
    mkdir(SD_ROOT "/icons", 0775);
    mkdir(SD_ROOT "/scenes", 0775);
    // eventos ainda por entregar de uma sessão anterior
    FILE *pf = fopen(SD_ROOT "/" PENDING, "r");
    if (pf) {
        int c, n = 0;
        while ((c = fgetc(pf)) != EOF) n += c == '\n';
        fclose(pf);
        s_pending = n;
    }
    s_jobs = xQueueCreate(32, sizeof(job_t));
    xTaskCreate(writer_task, "sdw", 4096, NULL, 2, NULL);   // stack interno (escreve no cartão)
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb);
    ESP_LOGI(TAG, "cartão %s: %llu MB, %llu MB livres", bsp_sdcard->cid.name, total >> 20, freeb >> 20);
}

bool sd_ok(void)
{
    return s_ok;
}

bool sd_path(const char *rel, char *out, size_t n)
{
    if (!rel || rel[0] == '/' || strstr(rel, "..") || strlen(rel) > 96) return false;
    snprintf(out, n, SD_ROOT "/%s", rel);
    return true;
}

// cria as pastas intermédias de um caminho absoluto
static void mkdirs(const char *abs)
{
    char tmp[160];
    strlcpy(tmp, abs, sizeof(tmp));
    for (char *p = tmp + strlen(SD_ROOT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
}

bool sd_write_text(const char *rel, const char *text)
{
    char p[160];
    if (!s_ok || !sd_path(rel, p, sizeof(p))) return false;
    mkdirs(p);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok;
}

char *sd_read_text(const char *rel, size_t max)
{
    char p[160];
    if (!s_ok || !sd_path(rel, p, sizeof(p))) return NULL;
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    char *buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (buf) buf[n] = '\0';
    return buf;
}

static void reply(link_via_t via, cJSON *o)
{
    link_send_via(o, via);
    cJSON_Delete(o);
}

static cJSON *status(const char *t, bool ok, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", t);
    cJSON_AddBoolToObject(o, "ok", ok);
    if (err) cJSON_AddStringToObject(o, "err", err);
    return o;
}

bool sd_handle(const cJSON *msg, link_via_t via)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    if (!t) return false;
    const char *rel = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "path"));
    char p[160];

    if (!strcmp(t, "sd?")) {
        cJSON *o = status("sd", s_ok, s_ok ? NULL : "sem cartão");
        if (s_ok) {
            uint64_t total = 0, freeb = 0;
            esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb);
            cJSON_AddStringToObject(o, "name", bsp_sdcard->cid.name);
            cJSON_AddNumberToObject(o, "total_mb", (double)(total >> 20));
            cJSON_AddNumberToObject(o, "free_mb", (double)(freeb >> 20));
            cJSON_AddNumberToObject(o, "mhz", bsp_sdcard->real_freq_khz / 1000);
        }
        reply(via, o);
        return true;
    }
    if (!strcmp(t, "ls")) {   // {"t":"ls","path":"vox"} -> {"t":"ls","items":[[nome, bytes, pasta?]...]}
        cJSON *o = status("ls", false, NULL);
        if (!s_ok || !sd_path(rel ? rel : "", p, sizeof(p))) {
            cJSON_AddStringToObject(o, "err", "caminho inválido ou sem cartão");
            reply(via, o);
            return true;
        }
        size_t L = strlen(p);
        if (L && p[L - 1] == '/') p[L - 1] = '\0';
        DIR *d = opendir(p);
        cJSON_AddStringToObject(o, "path", rel ? rel : "");
        cJSON *arr = cJSON_AddArrayToObject(o, "items");
        if (d) {
            cJSON_ReplaceItemInObject(o, "ok", cJSON_CreateTrue());
            struct dirent *e;
            int n = 0;
            while ((e = readdir(d)) && n++ < 200) {
                char fp[300];
                struct stat st = {0};
                snprintf(fp, sizeof(fp), "%s/%s", p, e->d_name);
                stat(fp, &st);
                cJSON *it = cJSON_CreateArray();
                cJSON_AddItemToArray(it, cJSON_CreateString(e->d_name));
                cJSON_AddItemToArray(it, cJSON_CreateNumber((double)st.st_size));
                cJSON_AddItemToArray(it, cJSON_CreateBool(S_ISDIR(st.st_mode)));
                cJSON_AddItemToArray(arr, it);
            }
            closedir(d);
        }
        reply(via, o);
        return true;
    }
    if (!strcmp(t, "file")) {   // {"t":"file","path":"vox/x.json","data":"<base64>","append":false}
        cJSON *o = status("file", false, NULL);
        const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "data"));
        if (!s_ok || !sd_path(rel, p, sizeof(p)) || !b64) {
            cJSON_AddStringToObject(o, "err", "caminho/dados inválidos ou sem cartão");
            reply(via, o);
            return true;
        }
        size_t blen = strlen(b64), olen = 0;
        unsigned char *raw = heap_caps_malloc(blen * 3 / 4 + 4, MALLOC_CAP_SPIRAM);
        if (!raw || mbedtls_base64_decode(raw, blen * 3 / 4 + 4, &olen, (const unsigned char *)b64, blen) != 0) {
            heap_caps_free(raw);
            cJSON_AddStringToObject(o, "err", "base64 inválido");
            reply(via, o);
            return true;
        }
        mkdirs(p);
        FILE *f = fopen(p, cJSON_IsTrue(cJSON_GetObjectItem(msg, "append")) ? "ab" : "wb");
        bool ok = f && fwrite(raw, 1, olen, f) == olen;
        if (f) fclose(f);
        heap_caps_free(raw);
        struct stat st = {0};
        stat(p, &st);
        cJSON_ReplaceItemInObject(o, "ok", cJSON_CreateBool(ok));
        cJSON_AddStringToObject(o, "path", rel);
        cJSON_AddNumberToObject(o, "size", (double)st.st_size);
        if (!ok) cJSON_AddStringToObject(o, "err", strerror(errno));
        reply(via, o);
        return true;
    }
    if (!strcmp(t, "rm")) {
        bool ok = s_ok && sd_path(rel, p, sizeof(p)) && (unlink(p) == 0 || rmdir(p) == 0);
        cJSON *o = status("rm", ok, ok ? NULL : "não apagado");
        if (rel) cJSON_AddStringToObject(o, "path", rel);
        reply(via, o);
        return true;
    }
    return false;
}
