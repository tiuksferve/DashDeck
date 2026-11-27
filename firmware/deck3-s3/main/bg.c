// Imagens de fundo do dashd (ver bg.h).
#include "bg.h"
#include "anim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "link.h"
#include "mbedtls/base64.h"

static const char *TAG = "bg";

// slots[0] = 4 bpp (mapas), slots[1] = 8 bpp (relógio); "pend" = a que está a chegar
static bgimg_t s_slot[2], s_pend;
static size_t s_pend_size;
static bool s_pend_ok;

static uint32_t hexcol(const char *s)
{
    return s && s[0] == '#' ? (uint32_t)strtoul(s + 1, NULL, 16) : 0;
}

static void ack(int off, bool ok)
{
    char b[64];
    snprintf(b, sizeof(b), "{\"t\":\"bgack\",\"off\":%d,\"ok\":%s}", off, ok ? "true" : "false");
    link_send_text(b);
}

void bg_handle(const cJSON *msg)
{
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "key"));
    const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "d"));
    const cJSON *jo = cJSON_GetObjectItem(msg, "off");
    int off = cJSON_IsNumber(jo) ? jo->valueint : -1;
    if (!key || !d || off < 0) {
        ack(off, false);
        return;
    }
    if (off == 0) {   // primeiro pedaço: tamanho, paleta e memória nova
        const cJSON *jw = cJSON_GetObjectItem(msg, "w"), *jh = cJSON_GetObjectItem(msg, "h");
        const cJSON *jb = cJSON_GetObjectItem(msg, "bpp"), *pal = cJSON_GetObjectItem(msg, "pal");
        heap_caps_free(s_pend.data);
        memset(&s_pend, 0, sizeof(s_pend));
        s_pend_ok = false;
        if (!cJSON_IsNumber(jw) || !cJSON_IsNumber(jh) || jw->valueint <= 0 || jh->valueint <= 0 ||
            jw->valueint > 960 || jh->valueint > 960) {
            ack(0, false);
            return;
        }
        strlcpy(s_pend.key, key, sizeof(s_pend.key));
        s_pend.w = jw->valueint;
        s_pend.h = jh->valueint;
        s_pend.bpp = cJSON_IsNumber(jb) && jb->valueint == 8 ? 8 : 4;
        for (int i = 0; i < 256 && i < cJSON_GetArraySize(pal); i++)
            s_pend.pal[i] = hexcol(cJSON_GetStringValue(cJSON_GetArrayItem(pal, i)));
        s_pend_size = s_pend.bpp == 8 ? (size_t)s_pend.w * s_pend.h : ((size_t)s_pend.w * s_pend.h + 1) / 2;
        s_pend.data = heap_caps_calloc(1, s_pend_size, MALLOC_CAP_SPIRAM);
        s_pend_ok = s_pend.data != NULL;
    }
    if (!s_pend_ok || strcmp(key, s_pend.key) != 0) {
        ack(off, false);
        return;
    }
    size_t olen = 0, room = (size_t)off < s_pend_size ? s_pend_size - off : 0;
    int r = mbedtls_base64_decode(s_pend.data + off, room, &olen, (const unsigned char *)d, strlen(d));
    if (r != 0) {
        ESP_LOGW(TAG, "pedaço %d de %s inválido (%d)", off, key, r);
        s_pend_ok = false;
        ack(off, false);
        return;
    }
    ack(off, true);
    if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "last"))) {   // completa: troca com o slot dessa profundidade
        bgimg_t *slot = &s_slot[s_pend.bpp == 8 ? 1 : 0];
        uint8_t *old = slot->data;
        bsp_display_lock(0);   // o LVGL pode estar a desenhar com a anterior
        anim_lock();           // e a tarefa dos quadros (ui.c) também
        *slot = s_pend;
        anim_unlock();
        bsp_display_unlock();
        heap_caps_free(old);
        memset(&s_pend, 0, sizeof(s_pend));
        s_pend_ok = false;
        ESP_LOGI(TAG, "fundo %s (%dx%d, %d bpp)", slot->key, slot->w, slot->h, slot->bpp);
    }
}

const bgimg_t *bg_get(const char *key)
{
    if (!key) return NULL;
    for (int i = 0; i < 2; i++)
        if (s_slot[i].data && !strcmp(s_slot[i].key, key)) return &s_slot[i];
    return NULL;
}
