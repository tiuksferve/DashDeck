#pragma once
// Imagens de fundo enviadas pelo dashd ({"t":"bg"}, o protocolo do DashDeck): tiles dos mapas (4 bpp, 16
// cores) e a paisagem do relógio (8 bpp, 256 cores). Chegam aos pedaços, cada um confirmado com
// {"t":"bgack","off":n,"ok":bool}, e ficam na PSRAM (uma por profundidade: o mapa e o relógio não se trocam).
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
    char key[48];
    int w, h, bpp;           // bpp 4 (2 px/byte, nibble alto primeiro) ou 8
    uint32_t pal[256];       // RGB888
    uint8_t *data;
} bgimg_t;

// {"t":"bg","key","off","d":"base64"; no 1.º pedaço "w","h","pal":[..], "bpp":8; no último "last":true}
void bg_handle(const cJSON *msg);
// A imagem completa com esta chave, ou NULL.
const bgimg_t *bg_get(const char *key);
// Índice da paleta do píxel (x, y).
static inline uint8_t bg_px(const bgimg_t *b, int x, int y)
{
    if (b->bpp == 8) return b->data[y * b->w + x];
    uint32_t i = (uint32_t)y * b->w + x;
    uint8_t v = b->data[i >> 1];
    return (i & 1) ? (v & 0x0F) : (v >> 4);
}
