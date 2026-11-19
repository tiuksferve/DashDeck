// Modo relógio em pixel art. Tudo é escrito direto no buffer RGB565 do canvas.
// Pré-visualização de referência: o mesmo desenho em JS (ver docs/uso.md, "Relógio").
#include "clockface.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t *B;
static int BW, BH, BS;

#define DEG "\x02"   // ° na fonte pixel
#define A_ACUTE "\x01"

static inline uint16_t c565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

static inline void put(int x, int y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < BW && y < BH) B[y * BS + x] = c;
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BW) w = BW - x;
    if (y + h > BH) h = BH - y;
    for (int j = 0; j < h; j++) {
        uint16_t *row = B + (y + j) * BS + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

// mistura a cor c (0xRRGGBB) no pixel com alfa a (0..256)
static void blend(int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= BW || y >= BH) return;
    uint16_t p = B[y * BS + x];
    int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
    int cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    r += ((cr - r) * a) >> 8;
    g += ((cg - g) * a) >> 8;
    b += ((cb - b) * a) >> 8;
    B[y * BS + x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------- fontes ----------

typedef struct { char c; const char *r[7]; } g5_t;
static const g5_t G5[] = {
    {'0', {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}},
    {'1', {"00100", "01100", "00100", "00100", "00100", "00100", "01110"}},
    {'2', {"01110", "10001", "00001", "00010", "00100", "01000", "11111"}},
    {'3', {"11111", "00010", "00100", "00010", "00001", "10001", "01110"}},
    {'4', {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}},
    {'5', {"11111", "10000", "11110", "00001", "00001", "10001", "01110"}},
    {'6', {"00110", "01000", "10000", "11110", "10001", "10001", "01110"}},
    {'7', {"11111", "00001", "00010", "00100", "01000", "01000", "01000"}},
    {'8', {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}},
    {'9', {"01110", "10001", "10001", "01111", "00001", "00010", "01100"}},
    {'A', {"01110", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'\x01', {"00010", "00100", "01110", "10001", "11111", "10001", "10001"}},
    {'B', {"11110", "10001", "10001", "11110", "10001", "10001", "11110"}},
    {'C', {"01110", "10001", "10000", "10000", "10000", "10001", "01110"}},
    {'D', {"11110", "10001", "10001", "10001", "10001", "10001", "11110"}},
    {'E', {"11111", "10000", "10000", "11110", "10000", "10000", "11111"}},
    {'F', {"11111", "10000", "10000", "11110", "10000", "10000", "10000"}},
    {'G', {"01110", "10001", "10000", "10111", "10001", "10001", "01111"}},
    {'H', {"10001", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'I', {"01110", "00100", "00100", "00100", "00100", "00100", "01110"}},
    {'J', {"00111", "00010", "00010", "00010", "00010", "10010", "01100"}},
    {'K', {"10001", "10010", "10100", "11000", "10100", "10010", "10001"}},
    {'L', {"10000", "10000", "10000", "10000", "10000", "10000", "11111"}},
    {'M', {"10001", "11011", "10101", "10101", "10001", "10001", "10001"}},
    {'N', {"10001", "10001", "11001", "10101", "10011", "10001", "10001"}},
    {'O', {"01110", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'P', {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}},
    {'Q', {"01110", "10001", "10001", "10001", "10101", "10010", "01101"}},
    {'R', {"11110", "10001", "10001", "11110", "10100", "10010", "10001"}},
    {'S', {"01111", "10000", "10000", "01110", "00001", "00001", "11110"}},
    {'T', {"11111", "00100", "00100", "00100", "00100", "00100", "00100"}},
    {'U', {"10001", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'V', {"10001", "10001", "10001", "10001", "10001", "01010", "00100"}},
    {'W', {"10001", "10001", "10001", "10101", "10101", "10101", "01010"}},
    {'X', {"10001", "10001", "01010", "00100", "01010", "10001", "10001"}},
    {'Y', {"10001", "10001", "10001", "01010", "00100", "00100", "00100"}},
    {'Z', {"11111", "00001", "00010", "00100", "01000", "10000", "11111"}},
    {':', {"00", "11", "11", "00", "11", "11", "00"}},
    {'\x02', {"0110", "1001", "1001", "0110", "0000", "0000", "0000"}},
    {'%', {"11001", "11010", "00010", "00100", "01000", "01011", "10011"}},
    {'-', {"000", "000", "000", "111", "000", "000", "000"}},
    {'^', {"00100", "01110", "10101", "00100", "00100", "00100", "00100"}},
    {'~', {"00100", "00100", "00100", "00100", "10101", "01110", "00100"}},
    {'/', {"00001", "00010", "00010", "00100", "01000", "01000", "10000"}},
    {'!', {"1", "1", "1", "1", "1", "0", "1"}},
    {',', {"00", "00", "00", "00", "00", "01", "10"}},
    {'*', {"00100", "00100", "01110", "01110", "11111", "11111", "01110"}},   // gota
    {' ', {"000", "000", "000", "000", "000", "000", "000"}},
};

typedef struct { char c; const char *r[5]; } g3_t;
static const g3_t G3[] = {
    {'0', {"111", "101", "101", "101", "111"}}, {'1', {"110", "010", "010", "010", "111"}},
    {'2', {"111", "001", "111", "100", "111"}}, {'3', {"111", "001", "111", "001", "111"}},
    {'4', {"101", "101", "111", "001", "001"}}, {'5', {"111", "100", "111", "001", "111"}},
    {'6', {"111", "100", "111", "101", "111"}}, {'7', {"111", "001", "001", "001", "001"}},
    {'8', {"111", "101", "111", "101", "111"}}, {'9', {"111", "101", "111", "001", "111"}},
    {'^', {"010", "111", "010", "010", "010"}}, {'~', {"010", "010", "010", "111", "010"}},
    {'*', {"010", "010", "111", "111", "010"}}, {'%', {"101", "001", "010", "100", "101"}},
    {'-', {"000", "000", "111", "000", "000"}}, {' ', {"000", "000", "000", "000", "000"}},
};

static const g5_t *g5(char c)
{
    for (size_t i = 0; i < sizeof(G5) / sizeof(G5[0]); i++)
        if (G5[i].c == c) return &G5[i];
    return &G5[sizeof(G5) / sizeof(G5[0]) - 1];
}

static const g3_t *g3(char c)
{
    for (size_t i = 0; i < sizeof(G3) / sizeof(G3[0]); i++)
        if (G3[i].c == c) return &G3[i];
    return &G3[sizeof(G3) / sizeof(G3[0]) - 1];
}

// texto 5x7 em escala s; out >= 0 desenha contorno de 1 px nessa cor. Devolve a largura.
static int text5(const char *s, int x, int y, int sc, uint16_t col, int out)
{
    for (int pass = out >= 0 ? 0 : 1; pass < 2; pass++) {
        int cx = x;
        for (const char *p = s; *p; p++) {
            const g5_t *g = g5(*p);
            int gw = (int)strlen(g->r[0]);
            for (int r = 0; r < 7; r++)
                for (int k = 0; k < gw; k++)
                    if (g->r[r][k] == '1') {
                        if (pass == 0) rect(cx + k * sc - 1, y + r * sc - 1, sc + 2, sc + 2, (uint16_t)out);
                        else rect(cx + k * sc, y + r * sc, sc, sc, col);
                    }
            cx += (gw + 1) * sc;
        }
        if (pass == 1) return cx - x - sc;
    }
    return 0;
}

static int tw5(const char *s, int sc)
{
    int w = 0;
    for (const char *p = s; *p; p++) w += ((int)strlen(g5(*p)->r[0]) + 1) * sc;
    return w - sc;
}

static int text3(const char *s, int x, int y, int sc, uint16_t col, int out)
{
    for (int pass = out >= 0 ? 0 : 1; pass < 2; pass++) {
        int cx = x;
        for (const char *p = s; *p; p++) {
            const g3_t *g = g3(*p);
            for (int r = 0; r < 5; r++)
                for (int k = 0; k < 3; k++)
                    if (g->r[r][k] == '1') {
                        if (pass == 0) rect(cx + k * sc - 1, y + r * sc - 1, sc + 2, sc + 2, (uint16_t)out);
                        else rect(cx + k * sc, y + r * sc, sc, sc, col);
                    }
            cx += 4 * sc;
        }
        if (pass == 1) return cx - x - sc;
    }
    return 0;
}

// dígito 3x5 em blocos (mostrador retro)
static void big3(char c, int x, int y, int cell, int gap, uint16_t col)
{
    const g3_t *g = g3(c);
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 3; k++)
            if (g->r[r][k] == '1') rect(x + k * cell, y + r * cell, cell - gap, cell - gap, col);
}

static void icon(const clock_state_t *st, int x, int y, int sc, int out, const uint16_t *mono)
{
    if (!st->has_icon) return;
    if (out >= 0)
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++)
                if (st->icon[j * 16 + i]) rect(x + i * sc - 1, y + j * sc - 1, sc + 2, sc + 2, (uint16_t)out);
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            uint8_t v = st->icon[j * 16 + i];
            if (!v || v > 7) continue;
            uint16_t c = mono ? mono[st->icon_role[v] == 2 ? 1 : 0] : st->icon_pal[v];
            rect(x + i * sc, y + j * sc, sc, sc, c);
        }
}

static const char *DOW[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "S" A_ACUTE "B"};
static const char *MON[] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN", "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};

// ---------- partículas (chuva, neve, trovoada) ----------

static uint32_t s_seed;
static float frand(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / 16777216.0f;
}

#define N_DROPS 150
#define N_FLAKES 90
static float s_drop[N_DROPS][3];
static float s_flake[N_FLAKES][4];
static bool s_part_init;

static void part_init(void)
{
    s_seed = 5;
    for (int i = 0; i < N_DROPS; i++) {
        s_drop[i][0] = frand() * 320;
        s_drop[i][1] = frand() * 320;
        s_drop[i][2] = 120 + frand() * 80;
    }
    s_seed = 6;
    for (int i = 0; i < N_FLAKES; i++) {
        s_flake[i][0] = frand() * 320;
        s_flake[i][1] = frand() * 320;
        s_flake[i][2] = 14 + frand() * 16;
        s_flake[i][3] = frand() * 6;
    }
    s_part_init = true;
}

static void particles(const clock_state_t *st, float t)
{
    if (!s_part_init) part_init();
    if (st->anim == CLK_ANIM_RAIN || st->anim == CLK_ANIM_STORM) {
        int n = st->anim == CLK_ANIM_STORM ? N_DROPS : 110;
        uint32_t rc = st->night ? 0x5A6888 : 0xC6D4EE;
        for (int i = 0; i < n; i++) {
            float y = fmodf(s_drop[i][1] + t * s_drop[i][2], (float)(BH + 6)) - 3;
            float x = fmodf(s_drop[i][0] * BW / 320.0f - y * 0.25f, (float)BW);
            if (x < 0) x += BW;
            for (int k = 0; k < 3; k++) blend((int)(x - k * 0.25f), (int)y + k, rc, 150);
        }
    } else if (st->anim == CLK_ANIM_SNOW) {
        uint16_t w = c565(0xFAFAFF), w2 = c565(0xDCE2F0);
        for (int i = 0; i < N_FLAKES; i++) {
            int y = (int)fmodf(s_flake[i][1] + t * s_flake[i][2], (float)BH);
            int x = (int)(s_flake[i][0] * BW / 320.0f + sinf(t * 1.4f + s_flake[i][3]) * 3);
            put(x, y, w);
            if (i % 4 == 0) { put(x + 1, y, w); put(x, y + 1, w); put(x + 1, y + 1, w2); }
        }
    }
    if (st->anim == CLK_ANIM_STORM) {
        float ph = fmodf(t, 5.0f);
        if (ph < 0.12f || (ph > 0.25f && ph < 0.32f)) {
            for (int y = 0; y < BH; y++)
                for (int x = 0; x < BW; x++) blend(x, y, 0xFFFFFF, 115);
            s_seed = (uint32_t)(t / 5) * 7919u + 3;
            float bx = BW * 0.2f + frand() * BW * 0.6f;
            int by = 30, bottom = BH - 60;
            uint16_t c1 = c565(0xFFFBE0), c2 = c565(0xFFE680);
            while (by < bottom) {
                float nx = bx + (frand() - 0.5f) * 10;
                for (int k = 0; k < 6; k++) {
                    int xx = (int)(bx + (nx - bx) * k / 6);
                    put(xx, by + k, c1);
                    put(xx + 1, by + k, c2);
                }
                bx = nx;
                by += 6;
            }
        }
    }
}

// ---------- mostradores ----------

static void face_landscape(const clock_state_t *st, const struct tm *tm, float t)
{
    particles(st, t);
    const uint16_t ink = c565(0xFFFFFF);
    const int out = c565(0x0E1020);
    const bool land = BW > BH;
    char buf[24];
    int x = 10;
    if (st->has_time) {
        snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
        x += text5(buf, x, 10, 5, ink, out) + 5;
        if (tm->tm_sec % 2 == 0) text5(":", x, 10, 5, ink, out);
        x += 15;
        snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
        x += text5(buf, x, 10, 5, ink, out) + 6;
        snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
        text5(buf, x, 31, 2, c565(0xFFE9A8), out);
        snprintf(buf, sizeof(buf), "%s %d %s", DOW[tm->tm_wday % 7], tm->tm_mday, MON[tm->tm_mon % 12]);
        text5(buf, 11, 54, 2, ink, out);
    } else {
        text5("--:--", 10, 10, 5, ink, out);
    }
    if (!st->has_wx) return;
    const int ix = land ? 198 : 10, iy = land ? 8 : 82;
    icon(st, ix, iy, 2, out, NULL);
    snprintf(buf, sizeof(buf), "%d" DEG, st->temp);
    text5(buf, ix + 38, iy + 4, 3, ink, out);
    int xx = ix, yy = iy + 38;
    snprintf(buf, sizeof(buf), "%d", st->tmax);
    xx += text3("^", xx, yy, 2, c565(0xFF9A7A), out) + 2;
    xx += text3(buf, xx, yy, 2, ink, out) + 8;
    snprintf(buf, sizeof(buf), "%d", st->tmin);
    xx += text3("~", xx, yy, 2, c565(0x8CC4FF), out) + 2;
    xx += text3(buf, xx, yy, 2, ink, out) + 8;
    snprintf(buf, sizeof(buf), "%d%%", st->rain);
    xx += text3("*", xx, yy, 2, c565(0x6FB6FF), out) + 2;
    text3(buf, xx, yy, 2, ink, out);
}

static const uint32_t LCD_PAL[3][4] = {   // fundo, apagado, aceso, meia cor
    {0x170F04, 0x2A1D0A, 0xFFB000, 0x8A5C00},
    {0x07120A, 0x12261A, 0x7DFF6A, 0x3C8A34},
    {0x051216, 0x0E252C, 0x5EF2FF, 0x2B7D88},
};

static void face_retro(const clock_state_t *st, const struct tm *tm)
{
    const uint32_t *P = LCD_PAL[(st->pal >= 0 && st->pal < 3) ? st->pal : 0];
    const uint16_t bg = c565(P[0]), ghost = c565(P[1]), on = c565(P[2]), dim = c565(P[3]);
    const uint16_t mono[2] = {on, dim};
    const bool land = BW > BH;
    char buf[32], dg[5];
    rect(0, 0, BW, BH, bg);
    if (st->has_time) snprintf(dg, sizeof(dg), "%02d%02d", tm->tm_hour, tm->tm_min);
    int sec = st->has_time ? tm->tm_sec : 0;
    char ss[3];
    snprintf(ss, sizeof(ss), "%02d", sec);

    if (land) {
        if (st->has_time) {
            snprintf(buf, sizeof(buf), "%s %d %s %d", DOW[tm->tm_wday % 7], tm->tm_mday, MON[tm->tm_mon % 12],
                     tm->tm_year + 1900);
            text5(buf, 12, 7, 2, on, -1);
        }
        for (int i = 0; i < 2; i++) {
            big3('8', 278 + i * 16, 4, 4, 1, ghost);
            if (st->has_time) big3(ss[i], 278 + i * 16, 4, 4, 1, dim);
        }
        const int cell = 18, gap = 3, y0 = 28, xs[4] = {23, 89, 179, 245};
        for (int i = 0; i < 4; i++) {
            big3('8', xs[i], y0, cell, gap, ghost);
            if (st->has_time) big3(dg[i], xs[i], y0, cell, gap, on);
        }
        bool col_on = st->has_time && sec % 2 == 0;
        rect(152, y0 + cell, cell - gap, cell - gap, col_on ? on : ghost);
        rect(152, y0 + 3 * cell, cell - gap, cell - gap, col_on ? on : ghost);
        for (int i = 0; i < 60; i++)
            rect(10 + i * 5, 123, 4, 4, i < sec ? dim : (i == sec && st->has_time) ? on : ghost);
        rect(10, 132, 300, 1, ghost);
        if (st->has_wx) {
            icon(st, 12, 136, 2, -1, mono);
            snprintf(buf, sizeof(buf), "%d" DEG, st->temp);
            text5(buf, 54, 141, 3, on, -1);
            int x = 124;
            x += text5("^", x, 137, 2, dim, -1) + 4;
            snprintf(buf, sizeof(buf), "%d", st->tmax);
            text5(buf, x, 137, 2, on, -1);
            x = 124;
            x += text5("~", x, 155, 2, dim, -1) + 4;
            snprintf(buf, sizeof(buf), "%d", st->tmin);
            text5(buf, x, 155, 2, on, -1);
            x = 182;
            x += text5("*", x, 146, 2, dim, -1) + 4;
            snprintf(buf, sizeof(buf), "%d%%", st->rain);
            text5(buf, x, 146, 2, on, -1);
            snprintf(buf, sizeof(buf), "%dKM/H", st->wind);
            text5(buf, 248, 146, 2, on, -1);
        } else {
            text5("SEM PREVISAO", 54, 146, 2, dim, -1);
        }
    } else {
        // vertical: horas por cima dos minutos
        if (st->has_time) {
            snprintf(buf, sizeof(buf), "%s %d %s", DOW[tm->tm_wday % 7], tm->tm_mday, MON[tm->tm_mon % 12]);
            text5(buf, 8, 8, 2, on, -1);
        }
        for (int i = 0; i < 2; i++) {
            big3('8', 134 + i * 16, 5, 4, 1, ghost);
            if (st->has_time) big3(ss[i], 134 + i * 16, 5, 4, 1, dim);
        }
        const int cell = 16, gap = 3, x0 = (BW - (6 * cell + 10)) / 2;
        for (int i = 0; i < 4; i++) {
            int x = x0 + (i % 2) * (3 * cell + 10), y = 36 + (i / 2) * 96;
            big3('8', x, y, cell, gap, ghost);
            if (st->has_time) big3(dg[i], x, y, cell, gap, on);
        }
        for (int i = 0; i < 60; i++)
            rect(11 + (i % 30) * 5, 222 + (i / 30) * 7, 4, 4, i < sec ? dim : (i == sec && st->has_time) ? on : ghost);
        rect(8, 240, BW - 16, 1, ghost);
        if (st->has_wx) {
            icon(st, 8, 248, 2, -1, mono);
            snprintf(buf, sizeof(buf), "%d" DEG, st->temp);
            text5(buf, 48, 252, 3, on, -1);
            int x = 116;
            x += text5("^", x, 248, 2, dim, -1) + 4;
            snprintf(buf, sizeof(buf), "%d", st->tmax);
            text5(buf, x, 248, 2, on, -1);
            x = 116;
            x += text5("~", x, 266, 2, dim, -1) + 4;
            snprintf(buf, sizeof(buf), "%d", st->tmin);
            text5(buf, x, 266, 2, on, -1);
            x = 8;
            x += text5("*", x, 292, 2, dim, -1) + 4;
            snprintf(buf, sizeof(buf), "%d%%", st->rain);
            text5(buf, x, 292, 2, on, -1);
            snprintf(buf, sizeof(buf), "%dKM/H", st->wind);
            text5(buf, 84, 292, 2, on, -1);
        } else {
            text5("SEM PREVISAO", 8, 260, 2, dim, -1);
        }
    }
    for (int y = 1; y < BH; y += 2)   // linhas de varrimento
        for (int x = 0; x < BW; x++) {
            uint16_t p = B[y * BS + x];
            int r = p >> 11, g = (p >> 5) & 0x3F, b = p & 0x1F;
            B[y * BS + x] = (uint16_t)(((r * 7 / 8) << 11) | ((g * 7 / 8) << 5) | (b * 7 / 8));
        }
}

static void overlays(const clock_state_t *st)
{
    if (st->show_bright) {
        int x0 = BW - 26;
        for (int y = 20; y < 152 && y < BH; y++)
            for (int x = x0; x < x0 + 20; x++) blend(x, y, 0x000000, 140);
        int n = st->bright / 10;
        for (int i = 0; i < 10; i++)
            rect(x0 + 4, 140 - i * 12, 12, 9, c565(i < n ? 0xFFFFFF : 0x5A5A64));
    }
    if (st->show_dots) {
        for (int i = 0; i < 2; i++)
            rect(BW / 2 - 9 + i * 12, BH - 8, 6, 4, c565(i == st->face ? 0xFFFFFF : 0x6E6E78));
    }
    if (st->show_exit) {   // caixa ao centro: o 2.º toque sai do relógio
        const char *l1 = "TOQUE DE NOVO", *l2 = "PARA SAIR";
        int bw = BW - 8 < 176 ? BW - 8 : 176, bh = 52, x0 = (BW - bw) / 2, y0 = (BH - bh) / 2 + 20;   // abaixo da data
        for (int y = y0; y < y0 + bh; y++)
            for (int x = x0; x < x0 + bw; x++) blend(x, y, 0x000000, 200);
        uint16_t edge = c565(0xFFFFFF);
        rect(x0, y0, bw, 2, edge);
        rect(x0, y0 + bh - 2, bw, 2, edge);
        rect(x0, y0, 2, bh, edge);
        rect(x0 + bw - 2, y0, 2, bh, edge);
        int w1 = tw5(l1, 2), w2 = tw5(l2, 2);
        text5(l1, (BW - w1) / 2, y0 + 9, 2, edge, -1);
        text5(l2, (BW - w2) / 2, y0 + 29, 2, c565(0xFFE9A8), -1);
    }
}

bool clockface_animated(const clock_state_t *st)
{
    return st->face == 0 && st->anim != CLK_ANIM_NONE;
}

void clockface_draw(uint16_t *buf, int stride_px, int w, int h, const clock_state_t *st, const struct tm *tm,
                    uint32_t ms)
{
    B = buf;
    BW = w;
    BH = h;
    BS = stride_px;
    float t = (float)(ms % 3600000u) / 1000.0f;
    if (st->face == 1) face_retro(st, tm);
    else face_landscape(st, tm, t);
    overlays(st);
}

int clockfont_width(const char *s, int sc)
{
    return tw5(s, sc);
}

int clockfont_cells(const char *s, int x, int y, int sc, int16_t (*out)[3], int max)
{
    int n = 0, cx = x, ci = 0;
    for (const char *p = s; *p; p++, ci++) {
        const g5_t *g = g5(*p);
        int gw = (int)strlen(g->r[0]);
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < gw; k++)
                if (g->r[r][k] == '1' && n < max) {
                    out[n][0] = (int16_t)(cx + k * sc);
                    out[n][1] = (int16_t)(y + r * sc);
                    out[n][2] = (int16_t)ci;
                    n++;
                }
        cx += (gw + 1) * sc;
    }
    return n;
}
