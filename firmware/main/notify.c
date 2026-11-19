// Aviso de mensagem direta, animação "Ondas LoRa": ondas de rádio fecham-se no centro, os pixels
// juntam-se num envelope verde que pulsa e o nome do remetente escreve-se por baixo.
// Desenho num canvas pequeno (NOTIFY_W x NOTIFY_H) ao centro; o resto (fundo escurecido, faixa com o
// texto) são objetos do LVGL em ui.c. Contas inteiras por pixel (o C6 não tem FPU).
#include "notify.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "clockface.h"

static uint16_t *B;     // plano de cor RGB565
static uint8_t *A;      // plano alfa (RGB565A8: transparente onde não há desenho)
static int BS;
#define W NOTIFY_W
#define H NOTIFY_H

static char s_name[16];
static int16_t s_cells[400][3];
static int s_ncells, s_nchars;

#define NPIX ((64 / 2) * (42 / 2))
static int16_t s_from[NPIX][2];
static uint8_t s_delay[NPIX];

static const uint32_t GREEN = 0x2EA043, LIGHT = 0x67EA94, DARK = 0x0E3B1C, INK = 0xE7FFE9;

static inline uint16_t c565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

static inline uint32_t mix(uint32_t a, uint32_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 256) return b;
    int r = ((a >> 16) & 255) + ((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t >> 8);
    int g = ((a >> 8) & 255) + ((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t >> 8);
    int bl = (a & 255) + ((((int)b & 255) - (int)(a & 255)) * t >> 8);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static inline void put(int x, int y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < W && y < H) {
        B[y * BS + x] = c;
        A[y * W + x] = 255;
    }
}

static void blend(int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    if (a > 256) a = 256;
    uint8_t *al = &A[y * W + x];
    if (*al == 0) {   // sobre transparente: a cor com a opacidade pedida
        B[y * BS + x] = c565(c);
        *al = (uint8_t)(a > 255 ? 255 : a);
        return;
    }
    if (*al < 255) *al = (uint8_t)(*al + ((255 - *al) * a >> 8));
    uint16_t p = B[y * BS + x];
    uint32_t old = (uint32_t)(((p >> 11) << 3) << 16 | (((p >> 5) & 0x3F) << 2) << 8 | ((p & 0x1F) << 3));
    B[y * BS + x] = c565(mix(old, c, a));
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(i, j, c);
}

static void rect_a(int x, int y, int w, int h, uint32_t c, int a)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) blend(i, j, c, a);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 512; guard++) {
        put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void ring(int cx, int cy, int r, uint32_t c, int a)
{
    int n = r * 7 < 30 ? 30 : r * 7;
    for (int k = 0; k < n; k++) {
        float t = (float)k / n * 6.2831853f;
        blend(cx + (int)lroundf(cosf(t) * r), cy + (int)lroundf(sinf(t) * r), c, a);
    }
}

static float clampf(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float ease(float t)
{
    t = clampf(t);
    float u = 1 - t;
    return 1 - u * u * u;
}

// envelope fechado, em pixel art, centrado em (cx, cy)
static void envelope(int cx, int cy, int w, int h, int glow)
{
    int x = cx - w / 2, y = cy - h / 2;
    for (int r = 4; glow && r > 0; r--) rect_a(x - r * 2, y - r * 2, w + r * 4, h + r * 4, LIGHT, glow * 18 / 256);
    rect(x, y, w, h, c565(GREEN));
    uint16_t dk = c565(DARK), flap = c565(mix(GREEN, LIGHT, 90));
    int my = y + h * 3 / 5;   // bico da aba
    for (int j = y; j <= my; j++) {   // aba: triângulo mais claro
        int half = (w / 2) * (my - j) / (my - y);
        rect(cx - half, j, half * 2, 1, flap);
    }
    line(x, y, cx, my, dk);
    line(x + w - 1, y, cx, my, dk);
    rect(x, y, w, 1, dk);
    rect(x, y + h - 1, w, 1, dk);
    rect(x, y, 1, h, dk);
    rect(x + w - 1, y, 1, h, dk);
    rect_a(x + 1, y + 1, w - 2, 1, LIGHT, 128);
}

static uint32_t s_seed;
static int rnd(int n)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (int)((s_seed >> 8) % (uint32_t)n);
}

void notify_start(const char *name)
{
    // nome em maiúsculas (a fonte pixel só tem maiúsculas); o que não existir fica em branco
    int n = 0;
    for (const char *p = name; *p && n < (int)sizeof(s_name) - 1; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((unsigned char)c >= 0x80) continue;   // UTF-8 (acentos, emojis): fora
        s_name[n++] = c;
    }
    s_name[n] = '\0';
    s_nchars = n;
    s_seed = 7;
    for (int i = 0; i < NPIX; i++) {
        s_from[i][0] = (int16_t)rnd(W);
        s_from[i][1] = (int16_t)rnd(H);
        s_delay[i] = (uint8_t)rnd(30);
    }
}

void notify_draw(uint8_t *buf, float t)
{
    B = (uint16_t *)buf;
    BS = W;
    A = buf + W * H * 2;
    const int cx = W / 2, cy = 46;
    memset(buf, 0, W * H * 3);
    // ondas a fechar sobre o centro
    for (int k = 0; k < 3; k++) {
        float p = clampf((t - k * 0.15f) / 0.8f);
        if (p > 0 && p < 1) ring(cx, cy, (int)(110 - 98 * ease(p)), LIGHT, (int)(205 * (1 - p * 0.5f)));
    }
    if (t < 1.25f) {
        // os pixels voam para a forma do envelope
        int i = 0;
        for (int y = 0; y < 42; y += 2)
            for (int x = 0; x < 64; x += 2, i++) {
                float e = ease((t - 0.45f - s_delay[i] / 100.0f) / 0.7f);
                if (e <= 0) continue;
                int px = (int)lroundf(s_from[i][0] + (cx - 32 + x - s_from[i][0]) * e);
                int py = (int)lroundf(s_from[i][1] + (cy - 21 + y - s_from[i][1]) * e);
                rect(px, py, 2, 2, c565(mix(LIGHT, GREEN, (int)(e * 256))));
            }
    } else {
        float k = clampf((t - 1.25f) / 0.3f);
        float pulse = 1 + sinf(k * 3.14159f) * 0.12f;
        envelope(cx, cy, (int)(64 * pulse), (int)(42 * pulse), 200);
        for (int j = 0; j < 2; j++) {
            float r = 30 + fmodf((t - 1.25f) * 80 + j * 30, 70);
            ring(cx, cy, (int)r, LIGHT, (int)(clampf(1 - (r - 30) / 70) * 180));
        }
    }
    // o nome escreve-se letra a letra
    if (t > 1.3f && s_nchars) {
        int shown = (int)((t - 1.3f) / 0.07f);
        if (shown > s_nchars) shown = s_nchars;
        char part[16];
        memcpy(part, s_name, (size_t)shown);
        part[shown] = '\0';
        int sc = clockfont_width(s_name, 2) <= W - 8 ? 2 : 1;
        int x0 = (W - clockfont_width(s_name, sc)) / 2, y0 = 82;
        s_ncells = clockfont_cells(part, x0, y0, sc, s_cells, 400);
        for (int i = 0; i < s_ncells; i++) rect(s_cells[i][0] - 1, s_cells[i][1] - 1, sc + 2, sc + 2, c565(0x0E1A12));
        for (int i = 0; i < s_ncells; i++) rect(s_cells[i][0], s_cells[i][1], sc, sc, c565(INK));
    }
}
