// Animações do DashDeck³ no estilo do DashDeck (ver anim.h): pixel art de jogo de 8 bits. As sequências
// seguem as do DashDeck (bootanim.c "Pixels", notify.c "Ondas LoRa", aboutanim.c "Hiperespaço"); as cenas
// grandes desenham-se a 240x240 (s_lo) e ampliam-se 2x (píxeis grossos).
#include "anim.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ------------------------------------------------------------------------------------- comum

static uint32_t *B;
static int BW, BH, BS;

#define LO 240                 // lado do buffer baixo (ampliado 2x)
static uint32_t *s_lo;
static void up2(uint32_t *dst, int w, int h, int stride);
static void cube8(int cx, int cy, int L, float z0, float z1, int ps);
static float bounce(float t);
static const uint32_t CUBE8[3] = {0x2DE2FF, 0xFF3D9A, 0xFFC21A};   // cores do cubo: ciano, magenta, âmbar

static inline uint32_t mix(uint32_t a, uint32_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 256) return b;
    int r = ((a >> 16) & 255) + ((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t >> 8);
    int g = ((a >> 8) & 255) + ((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t >> 8);
    int bl = (a & 255) + ((((int)b & 255) - (int)(a & 255)) * t >> 8);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static inline void put(int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < BW && y < BH) B[y * BS + x] = 0xFF000000u | c;
}

// mistura com alfa a (0..256); sobre píxeis transparentes (avisos) a cor fica com essa opacidade
static void blend(int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= BW || y >= BH || a <= 0) return;
    if (a > 256) a = 256;
    uint32_t *p = &B[y * BS + x];
    uint32_t pa = *p >> 24;
    if (pa == 0) {
        *p = ((uint32_t)(a > 255 ? 255 : a) << 24) | (c & 0xFFFFFF);
        return;
    }
    uint32_t na = pa + ((255 - pa) * (uint32_t)a >> 8);
    *p = (na << 24) | mix(*p & 0xFFFFFF, c, a);
}

static void rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(i, j, c);
}

static void fill(uint32_t c)
{
    uint32_t v = 0xFF000000u | c;
    for (int y = 0; y < BH; y++) {
        uint32_t *row = &B[y * BS];
        for (int x = 0; x < BW; x++) row[x] = v;
    }
}

static void ring(int cx, int cy, int r, uint32_t c, int a)
{
    int n = r * 7 < 30 ? 30 : r * 7;
    for (int k = 0; k < n; k++) {
        float t = (float)k / n * 6.2831853f;
        int x = cx + (int)lroundf(cosf(t) * r), y = cy + (int)lroundf(sinf(t) * r);
        blend(x, y, c, a);
        blend(x + 1, y, c, a / 2);   // 2 px: lê-se melhor nos 480x480
    }
}

static void line_a(float x0, float y0, float x1, float y1, uint32_t c, int a)
{
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)(fmaxf(fabsf(dx), fabsf(dy))) + 1;
    if (n > 600) n = 600;
    for (int i = 0; i <= n; i++) {
        float k = (float)i / n;
        blend((int)(x0 + dx * k), (int)(y0 + dy * k), c, a * (64 + 192 * i / n) / 256);   // cauda mais fraca
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

static uint32_t s_seed;
static float frand(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / 16777216.0f;
}

// paleta do DashDeck (ciano → azul → violeta → rosa → laranja → âmbar)
static const uint32_t PAL[8] = {0x5EF2FF, 0x58A6FF, 0x8B7CFF, 0xD2A8FF, 0xFF7AB6, 0xFF9A5A, 0xFFB000, 0xFFD64D};

// ------------------------------------------------------------------------------------- fonte

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
    {'\x03', {"111", "001", "111", "001", "111", "000", "000"}},   // ³ (expoente)
    {'\x02', {"010", "101", "010", "000", "000", "000", "000"}},   // °
    {':', {"00", "11", "11", "00", "11", "11", "00"}},
    {'.', {"00", "00", "00", "00", "00", "11", "11"}},
    {'%', {"11001", "11010", "00010", "00100", "01000", "01011", "10011"}},
    {'-', {"000", "000", "000", "111", "000", "000", "000"}},
    {'/', {"00001", "00010", "00010", "00100", "01000", "01000", "10000"}},
    {'!', {"1", "1", "1", "1", "1", "0", "1"}},
    {'?', {"01110", "10001", "00001", "00110", "00100", "00000", "00100"}},
    {',', {"00", "00", "00", "00", "00", "01", "10"}},
    {'(', {"01", "10", "10", "10", "10", "10", "01"}},
    {')', {"10", "01", "01", "01", "01", "01", "10"}},
    {' ', {"000", "000", "000", "000", "000", "000", "000"}},
};

static const g5_t *g5(char c)
{
    for (size_t i = 0; i < sizeof(G5) / sizeof(G5[0]); i++)
        if (G5[i].c == c) return &G5[i];
    return &G5[sizeof(G5) / sizeof(G5[0]) - 1];
}

// UTF-8 -> carateres da fonte: minúsculas em maiúsculas, acentos na letra base, ³ = '\x03'
static int fold(const char *s, char *out, int max)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max - 1) {
        unsigned c = *p++;
        if (c == 0xC2 && *p == 0xB3) { p++; out[n++] = '\x03'; continue; }
        if (c == 0xC2 && *p == 0xB0) { p++; out[n++] = '\x02'; continue; }   // °
        if (c == 0xC3 && *p) {
            unsigned d = *p++ | 0x20;   // minúscula
            char b = d >= 0xE0 && d <= 0xE5 ? 'A' : d == 0xE7 ? 'C' : d >= 0xE8 && d <= 0xEB ? 'E'
                   : d >= 0xEC && d <= 0xEF ? 'I' : d >= 0xF2 && d <= 0xF6 ? 'O' : d >= 0xF9 && d <= 0xFC ? 'U' : ' ';
            out[n++] = b;
            continue;
        }
        if (c >= 0x80) {   // outros UTF-8 (emojis...): salta a sequência
            while ((*p & 0xC0) == 0x80) p++;
            continue;
        }
        if (c >= 'a' && c <= 'z') c -= 32;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
}

// o ³ desenha-se mais pequeno (3/5) e em cima, como um expoente
#define SUP 0x100
static int s_sup_fix;   // > 0: tamanho fixo do bloco do ³ (o do arranque é maior que o normal)
static inline int sup_sc(int sc)
{
    if (s_sup_fix) return s_sup_fix;
    int s = sc * 3 / 5;
    return s < 1 ? 1 : s;
}

// tamanho do bloco de uma célula
static inline int cell_sz(int sc, int flag)
{
    return flag & SUP ? sup_sc(sc) : sc;
}

int pfont_width(const char *s, int sc)
{
    char f[96];
    fold(s, f, sizeof(f));
    int w = 0;
    for (const char *p = f; *p; p++) w += (int)strlen(g5(*p)->r[0]) * (*p == '\x03' ? sup_sc(sc) : sc) + sc;
    return w > 0 ? w - sc : 0;
}

// células acesas (x, y, índice do caráter | SUP) de um texto já dobrado
static int cells(const char *f, int x, int y, int sc, int16_t (*out)[3], int max)
{
    int n = 0, cx = x, ci = 0;
    for (const char *p = f; *p; p++, ci++) {
        const g5_t *g = g5(*p);
        int gw = (int)strlen(g->r[0]);
        bool sup = *p == '\x03';
        int s2 = sup ? sup_sc(sc) : sc;
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < gw; k++)
                if (g->r[r][k] == '1' && n < max) {
                    out[n][0] = (int16_t)(cx + k * s2);
                    out[n][1] = (int16_t)(y + r * s2);
                    out[n][2] = (int16_t)(ci | (sup ? SUP : 0));
                    n++;
                }
        cx += gw * s2 + sc;
    }
    return n;
}

static int16_t s_tmp[1200][3];

void pfont_draw(uint32_t *buf, int w, int h, int stride, const char *s, int x, int y, int sc, uint32_t col,
                uint32_t outline, int nchars)
{
    B = buf; BW = w; BH = h; BS = stride;
    char f[96];
    int len = fold(s, f, sizeof(f));
    if (nchars >= 0 && nchars < len) f[nchars] = '\0';
    int n = cells(f, x, y, sc, s_tmp, 1200);
    if (outline)
        for (int i = 0; i < n; i++) {
            int z = cell_sz(sc, s_tmp[i][2]);
            rect(s_tmp[i][0] - 1, s_tmp[i][1] - 1, z + 2, z + 2, outline);
        }
    for (int i = 0; i < n; i++) {
        int z = cell_sz(sc, s_tmp[i][2]);
        rect(s_tmp[i][0], s_tmp[i][1], z, z, col);
    }
}

// ------------------------------------------------------------------------------------- estrelas

#define NSTAR 90
static int16_t s_star[NSTAR][3];

static void stars_init(int w, int h)
{
    s_seed = 5;
    for (int i = 0; i < NSTAR; i++) {
        s_star[i][0] = (int16_t)(frand() * w);
        s_star[i][1] = (int16_t)(frand() * h);
        s_star[i][2] = (int16_t)(frand() * 60);
    }
}

static void stars_draw(float t, int amax)
{
    for (int i = 0; i < NSTAR; i++) {
        int a = (int)(amax / 2 + amax / 2 * sinf(t * 2.6f + s_star[i][2] * 0.1f));
        blend(s_star[i][0], s_star[i][1], 0xDCE6FF, a);
        if (s_star[i][2] % 9 == 0) {   // algumas maiores, em cruz
            blend(s_star[i][0] + 1, s_star[i][1], 0xDCE6FF, a / 3);
            blend(s_star[i][0] - 1, s_star[i][1], 0xDCE6FF, a / 3);
            blend(s_star[i][0], s_star[i][1] + 1, 0xDCE6FF, a / 3);
            blend(s_star[i][0], s_star[i][1] - 1, 0xDCE6FF, a / 3);
        }
    }
}

// ------------------------------------------------------------------------------------- arranque

#define LOGO "DASHDECK\xC2\xB3"
#define MAXC 400

// o nome a sair do cubo no "Sobre"
static int16_t s_cells[MAXC][3];
static int16_t s_from[MAXC][2];
static uint8_t s_delay[MAXC];
static int s_ncells, s_logo_y;

// ------------------------------------------------------------------------------------- avisos
// 8 bits: ondas em blocos a fechar sobre o centro, o sprite (envelope, despertador, avião) cai e assenta
// com ressalto e mexe-se conforme o tipo (a mensagem balança, o alarme abana, a emergência pisca); o nome
// escreve-se letra a letra por baixo.

static const uint32_t NCOL[3][4] = {
    {0x2EA043, 0x67EA94, 0x0E3B1C, 0xE7FFE9},   // mensagem: verde LoRa
    {0xF2B21B, 0xFFE27A, 0x4A2A00, 0xFFF6E0},   // alarme: dourado
    {0xDA3633, 0xFF7B72, 0x3D0808, 0xFFECEC},   // emergência: vermelho
};
static int s_nkind, s_nsprite;
static char s_nname[40];
static int s_nlen;

uint32_t anim_notify_color(int kind, int which)
{
    return NCOL[kind < 0 || kind > 2 ? 0 : kind][which & 3];
}

void anim_notify_start(int sprite, const char *name, int kind)
{
    s_nkind = kind < 0 || kind > 2 ? 0 : kind;
    s_nsprite = sprite < 0 || sprite > 2 ? 0 : sprite;
    char f[96];
    s_nlen = fold(name ? name : "", f, sizeof(f));
    if (s_nlen > (int)sizeof(s_nname) - 1) s_nlen = sizeof(s_nname) - 1;
    memcpy(s_nname, f, s_nlen);
    s_nname[s_nlen] = '\0';
}

int anim_sprite_id(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "mail") || !strcmp(name, "msg")) return SPR_MAIL;
    if (!strcmp(name, "clock") || !strcmp(name, "alarm")) return SPR_CLOCK;
    if (!strcmp(name, "plane") || !strcmp(name, "squawk")) return SPR_PLANE;
    return -1;
}

#define NS 8   // tamanho de um pixel do sprite no ecrã
#define INK 0x0A0A18
#define PAPER 0xF4F7FF

static const char *SPR_MAIL_ROWS[12] = {
    "oooooooooooooooo",
    "owwppppppppppwwo",
    "opwwppppppppwwpo",
    "oppwwppppppwwppo",
    "opppwwppppwwpppo",
    "oppppwwppwwppppo",
    "opppppwwwwpppppo",
    "oppppppwwppppppo",
    "oppppppppppppppo",
    "oppppppppppppppo",
    "oppppppppppppppo",
    "oooooooooooooooo",
};
static const char *SPR_PLANE_ROWS[13] = {
    ".......oo.......",
    "......owwo......",
    "......olwo......",
    ".....owwwwo.....",
    "...oowwwwwwoo...",
    ".oowwwwwwwwwwoo.",
    "owwwwwwwwwwwwwwo",
    "oooooowwwwoooooo",
    "......owwo......",
    "......owwo......",
    ".....owwwwo.....",
    "....owwwwwwo....",
    "....oooooooo....",
};

static inline void sp(int ox, int oy, int x, int y, uint32_t c)
{
    rect(ox + x * NS, oy + y * NS, NS, NS, c);
}

static void spr_rows(const char *const *rows, int nr, int ox, int oy, uint32_t w, uint32_t l)
{
    for (int y = 0; y < nr; y++)
        for (int x = 0; x < 16; x++) {
            char k = rows[y][x];
            if (k == '.') continue;
            sp(ox, oy, x, y, k == 'o' ? INK : k == 'w' ? w : k == 'l' ? l : PAPER);
        }
}

// despertador 16x16: duas campainhas, caixa redonda, mostrador, ponteiros e pés
static void spr_clock(int ox, int oy, uint32_t w, uint32_t l)
{
    static const int8_t BELL[2][2] = {{3, 2}, {12, 2}};
    for (int b = 0; b < 2; b++)
        for (int y = -2; y <= 2; y++)
            for (int x = -2; x <= 2; x++) {
                int d = x * x + y * y;
                if (d <= 5) sp(ox, oy, BELL[b][0] + x, BELL[b][1] + y, d >= 4 ? INK : l);
            }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            float dx = x - 7.5f, dy = y - 8.5f, d = dx * dx + dy * dy;
            if (d > 6.6f * 6.6f) continue;
            sp(ox, oy, x, y, d > 5.7f * 5.7f ? INK : d > 4.6f * 4.6f ? w : PAPER);
        }
    for (int k = 0; k < 4; k++) sp(ox, oy, 7, 8 - k, INK);   // ponteiros
    for (int k = 1; k < 3; k++) sp(ox, oy, 7 + k, 8, INK);
    sp(ox, oy, 3, 15, INK); sp(ox, oy, 12, 15, INK);         // pés
    sp(ox, oy, 7, 1, INK); sp(ox, oy, 8, 1, INK);             // martelo
}

// círculo em blocos de 4 px (ondas de 8 bits) sobre o canvas transparente
static void ring8(int cx, int cy, int r, uint32_t c, int a)
{
    if (r < 2 || a <= 0) return;
    int n = (int)(r * 1.6f) + 8, lx = -99, ly = -99;
    for (int k = 0; k < n; k++) {
        float t = (float)k / n * 6.2831853f;
        int x = (cx + (int)lroundf(cosf(t) * r)) & ~3, y = (cy + (int)lroundf(sinf(t) * r)) & ~3;
        if (x == lx && y == ly) continue;
        lx = x;
        ly = y;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) blend(x + i, y + j, c, a);
    }
}

void anim_notify_draw(uint32_t *buf, int w, int h, int stride, float t)
{
    B = buf; BW = w; BH = h; BS = stride;
    for (int y = 0; y < h; y++) memset(&buf[y * stride], 0, w * 4);   // transparente
    const uint32_t *C = NCOL[s_nkind];
    int cx = w / 2, cy = h * 42 / 100;
    for (int k = 0; k < 3; k++) {   // ondas a fechar sobre o centro
        float p = clampf((t - k * 0.15f) / 0.8f);
        if (p > 0 && p < 1) ring8(cx, cy, (int)(160 - 135 * ease(p)), C[1], (int)(205 * (1 - p * 0.5f)));
    }
    float k = (t - 0.35f) / 0.6f;   // o sprite cai e assenta com ressalto
    if (k < 0) return;
    int nr = s_nsprite == SPR_MAIL ? 12 : s_nsprite == SPR_PLANE ? 13 : 16;
    int sw = 16 * NS, sh = nr * NS;
    int ox = cx - sw / 2, oy0 = cy - sh / 2;
    int oy = oy0 - (int)lroundf((1 - bounce(k)) * (oy0 + sh));
    uint32_t main = C[0], light = C[1];
    if (k >= 1) {
        float kt = t - 0.95f;
        if (s_nkind == 2) {                                  // emergência: pisca 3 vezes por segundo
            if (((int)(kt * 6)) % 2 == 0) { main = C[1]; light = 0xFFFFFF; }
        } else if (s_nkind == 1) {                           // alarme: abana em rajadas de 0,6 s
            if (fmodf(kt, 1.0f) < 0.6f) ox += ((int)(t * 20)) % 2 ? NS / 2 : -NS / 2;
        } else {                                             // mensagem: balança aos degraus
            oy += ((int)lroundf(sinf(kt * 4) * 1.5f)) * (NS / 2);
        }
        for (int j = 0; j < 2; j++) {                        // ondas a sair
            float r = fmodf(kt * 80 + j * 40, 80);
            ring8(cx, cy, 84 + (int)r, C[1], (int)(clampf(1 - r / 80) * 170));
        }
    }
    if (s_nsprite == SPR_CLOCK) spr_clock(ox, oy, main, light);
    else if (s_nsprite == SPR_PLANE) spr_rows(SPR_PLANE_ROWS, 13, ox, oy, main, light);
    else spr_rows(SPR_MAIL_ROWS, 12, ox, oy, main, light);
    // o nome escreve-se letra a letra por baixo
    if (t > 1.3f && s_nlen) {
        int shown = (int)((t - 1.3f) / 0.07f);
        int sc = pfont_width(s_nname, 3) <= w - 8 ? 3 : 2;
        pfont_draw(buf, w, h, stride, s_nname, (w - pfont_width(s_nname, sc)) / 2, h - 7 * sc - 6, sc, C[3],
                   0x0E1A12, shown);
    }
}

// Ícone de reserva das boxes (sem imagem do dashd): placa de 8 bits na cor do item com a inicial.
void anim_badge(uint32_t *buf, int size, int stride, uint32_t col, const char *label)
{
    B = buf; BW = size; BH = size; BS = stride;
    for (int y = 0; y < size; y++) memset(&buf[y * stride], 0, size * 4);
    const int ps = 4, n = 18, o = (size - n * ps) / 2;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            bool edge_i = i == 0 || i == n - 1, edge_j = j == 0 || j == n - 1;
            if (edge_i && edge_j) continue;   // cantos cortados
            uint32_t c = edge_i || edge_j ? INK : j <= 2 ? mix(col, 0xFFFFFF, 90) : j >= n - 3 ? mix(col, 0, 90) : col;
            rect(o + i * ps, o + j * ps, ps, ps, c);
        }
    char f[8];
    fold(label && *label ? label : "?", f, sizeof(f));
    f[1] = '\0';
    pfont_draw(buf, size, size, stride, f, (size - pfont_width(f, 6)) / 2, (size - 7 * 6) / 2, 6, 0xFFFFFF, INK, -1);
}

// ------------------------------------------------------------------------------------- "Sobre"
// Salto no hiperespaço em 8 bits (desenhado a 240x240 e ampliado 2x): as estrelas esticam-se em riscos,
// clarão, as camadas do cubo caem e o nome sai do cubo.

#define NWARP 120
static float s_warp[NWARP][3];   // ângulo, distância inicial, velocidade

void anim_about_start(int w, int h)
{
    (void)w;
    (void)h;
    if (!s_lo) s_lo = heap_caps_malloc(LO * LO * 4, MALLOC_CAP_SPIRAM);
    BW = LO;
    BH = LO;
    stars_init(LO, LO);
    s_seed = 23;
    for (int i = 0; i < NWARP; i++) {
        s_warp[i][0] = frand() * 6.2831853f;
        s_warp[i][1] = 2 + frand() * 30;
        s_warp[i][2] = 0.6f + frand() * 0.8f;
    }
    char f[16];
    fold(LOGO, f, sizeof(f));
    s_logo_y = LO * 63 / 100;
    s_sup_fix = 4;
    s_ncells = cells(f, (LO - pfont_width(LOGO, 4)) / 2, s_logo_y, 4, s_cells, MAXC);
    s_sup_fix = 0;
    s_seed = 31;
    for (int i = 0; i < s_ncells; i++) {
        s_from[i][0] = (int16_t)(LO / 2 + (frand() - 0.5f) * 20);   // os pixels saem do cubo
        s_from[i][1] = (int16_t)(LO * 0.36f + (frand() - 0.5f) * 20);
        s_delay[i] = (uint8_t)(frand() * 40);
    }
}

void anim_about_draw(uint32_t *buf, int stride, float t, float tilt)
{
    (void)tilt;
    if (!s_lo) return;
    B = s_lo; BW = LO; BH = LO; BS = LO;
    fill(0x000000);
    int cx = LO / 2, cy = LO * 36 / 100;
    const float JUMP = 1.7f;
    if (t < JUMP) {
        float acc = t * t * 1.4f;
        for (int i = 0; i < NWARP; i++) {
            float a = s_warp[i][0], d0 = s_warp[i][1], sp2 = s_warp[i][2];
            float d = fmodf(d0 + acc * 130 * sp2, 180);
            float len = 2 + acc * 60 * sp2;
            float cs = cosf(a), sn = sinf(a);
            uint32_t c = i % 5 == 0 ? 0xA8C8FF : i % 7 == 0 ? 0xD2A8FF : 0xFFFFFF;
            line_a(cx + cs * d, LO / 2 + sn * d, cx + cs * (d + len), LO / 2 + sn * (d + len), c,
                   (int)(80 + 176 * clampf(t / 0.8f)));
        }
    } else {
        float k = t - JUMP;
        stars_draw(t, (int)(150 * clampf(k / 0.6f)));
        for (int j = 0; j < 3 && k > 1.4f; j++) {   // ondas do cubo montado
            float r = fmodf((k - 1.4f) * 35 + j * 25, 75);
            ring(cx, cy, 32 + (int)r, PAL[(j * 3 + 1) % 8], (int)(clampf(1 - r / 75) * 120));
        }
        int bob = k > 1.4f ? (int)lroundf(sinf(k * 2) * 2) : 0;
        for (int s3 = 0; s3 < 3; s3++) {   // as camadas caem
            float e = bounce((k - 0.2f - s3 * 0.3f) / 0.55f);
            if (k < 0.2f + s3 * 0.3f) break;
            cube8(cx, cy + bob - (int)lroundf((1 - e) * (cy + 30)), 22, s3 / 3.0f, (s3 + 1) / 3.0f, 1);
        }
        for (int i = 0; i < s_ncells; i++) {   // o nome sai do cubo
            float e = ease((k - 1.2f - s_delay[i] / 100.0f) / 1.0f);
            if (e <= 0) continue;
            int x = (int)lroundf(s_from[i][0] + (s_cells[i][0] - s_from[i][0]) * e);
            int y = (int)lroundf(s_from[i][1] + (s_cells[i][1] - s_from[i][1]) * e);
            uint32_t c = s_cells[i][2] & SUP ? CUBE8[(((s_cells[i][1] - s_logo_y) / 4 + 1) / 2) % 3]   // ³ em faixas
                                             : PAL[((s_cells[i][2] & 0xFF) * 2) % 8];
            int z = e < 1 ? 2 + (int)(e * 2) : 4;
            if (e >= 1) rect(x + 1, y + 1, 4, 4, 0x05040E);
            rect(x, y, z, z, e < 1 ? mix(0, c, (int)(100 + 156 * e)) : c);
        }
        if (k < 0.35f) {   // clarão no fim do salto
            int a = (int)((1 - k / 0.35f) * 230);
            for (int y = 0; y < LO; y++)
                for (int x = 0; x < LO; x++) blend(x, y, 0xFFFFFF, a);
        }
    }
    up2(buf, LO * 2, LO * 2, stride);
}

// ------------------------------------------------------------------------------------- fora de casa

void anim_away_draw(uint32_t *buf, int w, int h, int stride, float t, const away_t *a)
{
    B = buf; BW = w; BH = h; BS = stride;
    fill(0x000000);   // AMOLED: preto = píxeis apagados
    static bool init;
    if (!init) {
        stars_init(w, h);
        init = true;
    }
    stars_draw(t, 70);
    int dx = (a->shift % 3 - 1) * 6, dy = (a->shift / 3 - 1) * 5;
    int cx = w / 2 + dx;
    // o cubo de 8 bits (blocos de 2 px) a flutuar devagar
    cube8(cx, 62 + dy + (int)lroundf(sinf(t * 1.5f) * 2) * 2, 14, 0, 1, 2);
    B = buf; BW = w; BH = h; BS = stride;
    // hora grande, cada dígito com a sua cor da paleta
    int sc = a->pending < 0 ? 10 : 14;   // no relógio do uConsole, mais pequeno
    int tw = pfont_width(a->time, sc), x = cx - tw / 2, y = (a->pending < 0 ? 136 : 120) + dy;
    char one[2] = {0, 0};
    for (const char *p = a->time; *p; p++) {
        one[0] = *p;
        uint32_t c = *p == ':' ? ((int)(t * 2) % 2 ? 0x9AA4C0 : 0x3A4460) : PAL[((p - a->time) * 2) % 8];
        pfont_draw(buf, w, h, stride, one, x, y, sc, c, 0x0E1020, -1);
        x += pfont_width(one, sc) + sc;
    }
    B = buf; BS = stride;
    pfont_draw(buf, w, h, stride, a->date, cx - pfont_width(a->date, 4) / 2, y + 7 * sc + 20, 4, 0xB8C2D0, 0, -1);
    // bateria: barra em 10 blocos com a percentagem
    int by = y + 7 * sc + 70;
    if (a->bat >= 0) {
        uint32_t c = a->chg ? 0x4BE38A : a->bat > 40 ? 0x4BE38A : a->bat > 15 ? 0xFFB23E : 0xFF5470;
        int bx = cx - 130;
        for (int i = 0; i < 10; i++) {
            bool on = a->bat >= i * 10 + 5;
            uint32_t cc = on ? c : 0x1A2030;
            if (on && a->chg && i == (int)(t * 4) % 10) cc = mix(c, 0xFFFFFF, 140);   // a carregar: um bloco corre
            rect(bx + i * 20, by, 16, 16, cc);
            rect(bx + i * 20, by, 16, 3, mix(cc, 0xFFFFFF, 90));                       // brilho de cima
            rect(bx + i * 20, by + 13, 16, 3, mix(cc, 0x000000, 110));                 // sombra
        }
        char b[16];
        snprintf(b, sizeof(b), "%d%%", a->bat);
        pfont_draw(buf, w, h, stride, b, bx + 210, by, 2, 0xE8F0FF, 0, -1);
        B = buf; BS = stride;
    }
    // eventos guardados e estado
    char ev[48];
    if (a->pending < 0) ev[0] = '\0';   // relógio do uConsole: sem a linha dos eventos
    else if (!a->sd) snprintf(ev, sizeof(ev), "SEM CARTAO: EVENTOS NAO GUARDADOS");
    else if (a->pending == 1) snprintf(ev, sizeof(ev), "1 EVENTO GUARDADO NO CARTAO");
    else snprintf(ev, sizeof(ev), "%d EVENTOS GUARDADOS NO CARTAO", a->pending);
    if (ev[0]) pfont_draw(buf, w, h, stride, ev, cx - pfont_width(ev, 2) / 2, by + 38, 2, a->pending ? 0xFFD64D : 0x6A7488, 0, -1);
    if (a->status && *a->status) {
        int s2 = pfont_width(a->status, 2) <= w - 16 ? 2 : 1;
        pfont_draw(buf, w, h, stride, a->status, cx - pfont_width(a->status, s2) / 2, h - 30, s2, 0x6A7488, 0, -1);
    }
}

// ------------------------------------------------------------------------------------- relógio
// Faces do relógio do uConsole no cubo (tpl "clock"): 0 paisagem (a cena do DashDeck, desenhada pelo dashd a
// 240x240 e enviada como fundo, com a chuva/neve e o cão por cima), 1 aurora boreal (a cena "Sobre" do
// DashDeck), 2 o cubo, 3 temporizador e alarme. As cenas desenham-se a meia resolução (pixel art como no C6)
// e ampliam-se 2x; a hora, a data e o tempo vão por cima em resolução total.

static int16_t s_cfar[LO], s_cmid[LO], s_cstar[70][3];

static void aurora_init(void)
{
    const int HZ = LO * 70 / 100;
    for (int x = 0; x < LO; x++) {
        float f = HZ - LO * 0.20f - (14 * sinf(x * .030f + 1.3f) + 8 * sinf(x * .081f + .4f) + 3 * sinf(x * .19f + 2));
        s_cfar[x] = (int16_t)(lroundf(f / 2) * 2);
        s_cmid[x] = (int16_t)lroundf(HZ - 10 - (5 * sinf(x * .047f + 2.1f) + 3 * sinf(x * .12f) + 2 * sinf(x * .28f + 1)));
    }
    uint32_t seed = 11;
    for (int i = 0; i < 70; i++) {
        seed = seed * 1664525u + 1013904223u;
        s_cstar[i][0] = (int16_t)((seed >> 8) % LO);
        seed = seed * 1664525u + 1013904223u;
        s_cstar[i][1] = (int16_t)((seed >> 8) % (HZ - 30));
        s_cstar[i][2] = (int16_t)((seed >> 4) % 628);
    }
}

static void aurora_scene(float t)
{
    const int HZ = LO * 70 / 100;
    for (int y = 0; y < HZ; y++) {
        int f = y * 12 * 256 / HZ, b = f >> 8, fr = f & 255;
        uint32_t c0 = mix(0x050818, 0x1A1440, b * 256 / 12), c1 = mix(0x050818, 0x1A1440, (b + 1) * 256 / 12);
        uint32_t *row = &B[y * BS];
        for (int x = 0; x < LO; x++) row[x] = 0xFF000000u | ((fr > 128 && ((x + y) & 1) == 0) ? c1 : c0);
    }
    for (int i = 0; i < 70; i++)
        blend(s_cstar[i][0], s_cstar[i][1], 0xFFFFF0, (int)(140 + 100 * sinf(t * 2.3f + s_cstar[i][2] / 100.0f)));
    for (int x = 0; x < LO; x++) {   // cortinas verticais que ondulam, mais intensas em baixo
        float fx = (float)x * 1.333f;
        float base = HZ * 0.55f + 9 * sinf(fx * 0.018f + t * 0.7f) + 5 * sinf(fx * 0.047f - t * 1.1f);
        float inten = 0.55f + 0.45f * sinf(fx * 0.031f + t * 0.9f) * sinf(fx * 0.013f - t * 0.4f + 1);
        if (inten < 0.1f) continue;
        int hh = (int)(22 + 30 * inten), y0 = (int)base, ai = (int)(inten * 175);
        uint32_t c0 = mix(0x3CFFB0, 0x50E0FF, (int)(128 + 127 * sinf(fx * 0.02f + t)));
        for (int k = 0; k < hh; k++) {
            int y = y0 - k;
            if (y < 0) break;
            blend(x, y, k > hh * 2 / 3 ? 0xB070FF : c0, ai * (hh - k) / hh);
        }
        blend(x, y0 + 1, c0, ai / 3);
    }
    float ph = fmodf(t, 4.0f);   // estrela cadente a cada 4 s
    if (ph < 0.8f) {
        int k = (int)(t / 4.0f);
        float sx = LO * (0.15f + 0.6f * ((k * 37) % 10) / 10.0f), sy = HZ * 0.10f + (k * 13) % 18;
        float p = ph / 0.8f;
        for (int i = 0; i < 16; i++) {
            float q = p - i * 0.02f;
            if (q < 0) break;
            blend((int)(sx + q * 70), (int)(sy + q * 26), 0xFFFFFF, (int)(240 * (1 - i / 16.0f) * (1 - p * 0.5f)));
        }
    }
    for (int x = 0; x < LO; x++) {   // longe: azul escuro com neve no cimo; perto: colinas quase pretas
        int fh = s_cfar[x], mh = s_cmid[x];
        for (int y = fh; y < HZ; y++) {
            uint32_t c = y >= mh ? 0x070D10 : mix(0x0C1030, 0x080A1E, (y - fh) * 256 / (HZ - fh + 1));
            if (y < mh && y < fh + 5 && (x * 7 + y) % 5) c = mix(c, 0x8FA6C8, 120);
            B[y * BS + x] = 0xFF000000u | c;
        }
        put(x, fh, 0x4FA89A);
    }
    int cx = LO * 20 / 100, cy = HZ - 2;   // cabana com a janela acesa e fumo
    rect(cx - 8, cy - 9, 16, 9, 0x2A1A12);
    for (int i = 0; i < 10; i++) rect(cx - 10 + i, cy - 9 - i / 2, 20 - 2 * i, 1, 0x3A2416);
    rect(cx + 2, cy - 6, 4, 3, ((int)(t * 3) % 7) ? 0xFFC860 : 0xFFE0A0);
    for (int i = 0; i < 6; i++) {
        float q = fmodf(t * 0.5f + i / 6.0f, 1);
        blend(cx + 5 + (int)(4 * sinf(q * 6 + t)), cy - 15 - (int)(q * 26), 0x9098B0, (int)(120 * (1 - q)));
    }
    int phs = (int)(t * 3 * 256);   // lago: reflexo ondulado e mais escuro
    for (int y = HZ; y < LO; y++) {
        int sy2 = HZ - 1 - (y - HZ) * 2;
        if (sy2 < 0) sy2 = 0;
        int off = ((y * 230 + phs) / 256) % 3 - 1;
        for (int x = 0; x < LO; x++) {
            int xx = x + off < 0 ? 0 : x + off >= LO ? LO - 1 : x + off;
            uint32_t p = B[sy2 * BS + xx];
            int r = ((p >> 16) & 255) * 5 / 8, g = ((p >> 8) & 255) * 5 / 8, b = (p & 255) * 2 / 3 + 10;
            B[y * BS + x] = 0xFF000000u | (uint32_t)(r << 16 | g << 8 | (b > 255 ? 255 : b));
        }
    }
}

// ---- chuva, trovoada e neve (como no DashDeck), a meia resolução
#define N_DROPS 110
#define N_FLAKES 70
static float s_drop[N_DROPS][3], s_flake[N_FLAKES][4];
static bool s_part_init;
static uint32_t s_pseed;
static float prand(void)
{
    s_pseed = s_pseed * 1664525u + 1013904223u;
    return (float)(s_pseed >> 8) / 16777216.0f;
}

static void particles(int anim, bool night, float t)
{
    if (!s_part_init) {
        s_pseed = 5;
        for (int i = 0; i < N_DROPS; i++) {
            s_drop[i][0] = prand() * LO;
            s_drop[i][1] = prand() * LO;
            s_drop[i][2] = 90 + prand() * 60;
        }
        for (int i = 0; i < N_FLAKES; i++) {
            s_flake[i][0] = prand() * LO;
            s_flake[i][1] = prand() * LO;
            s_flake[i][2] = 10 + prand() * 12;
            s_flake[i][3] = prand() * 6;
        }
        s_part_init = true;
    }
    if (anim == CLK_RAIN || anim == CLK_STORM) {
        uint32_t rc = night ? 0x5A6888 : 0xC6D4EE;
        for (int i = 0; i < (anim == CLK_STORM ? N_DROPS : 80); i++) {
            float y = fmodf(s_drop[i][1] + t * s_drop[i][2], (float)(LO + 6)) - 3;
            float x = fmodf(s_drop[i][0] - y * 0.25f, (float)LO);
            if (x < 0) x += LO;
            for (int k = 0; k < 3; k++) blend((int)(x - k * 0.25f), (int)y + k, rc, 150);
        }
    } else if (anim == CLK_SNOW) {
        for (int i = 0; i < N_FLAKES; i++) {
            int y = (int)fmodf(s_flake[i][1] + t * s_flake[i][2], (float)LO);
            int x = (int)(s_flake[i][0] + sinf(t * 1.4f + s_flake[i][3]) * 3);
            put(x, y, 0xFAFAFF);
            if (i % 4 == 0) { put(x + 1, y, 0xFAFAFF); put(x, y + 1, 0xFAFAFF); put(x + 1, y + 1, 0xDCE2F0); }
        }
    }
    if (anim == CLK_STORM) {
        float ph = fmodf(t, 5.0f);
        if (ph < 0.12f || (ph > 0.25f && ph < 0.32f)) {
            for (int y = 0; y < LO; y++)
                for (int x = 0; x < LO; x++) blend(x, y, 0xFFFFFF, 110);
            s_pseed = (uint32_t)(t / 5) * 7919u + 3;
            float bx = LO * 0.2f + prand() * LO * 0.6f;
            for (int by = 20; by < LO - 40; by += 6) {
                float nx = bx + (prand() - 0.5f) * 10;
                for (int k = 0; k < 6; k++) {
                    int xx = (int)(bx + (nx - bx) * k / 6);
                    put(xx, by + k, 0xFFFBE0);
                    put(xx + 1, by + k, 0xFFE680);
                }
                bx = nx;
            }
        }
    }
}

// ---- o cão junto à casa (paisagem): o mesmo do DashDeck (firmware/main/clockface.c), a 240x240
// Em memória do Romeu, um schnauzer branco. De dia brinca com o dono, que lhe atira uma bola ou uma
// vareta: ele corre a buscá-la, traz de volta e senta-se à espera da próxima. À noite (e com chuva,
// neve ou nevoeiro) dorme enrolado ao lado da casa. Os sprites estão virados à direita.
// '#' pelo branco, 'g' barba e sobrancelhas, 'e' olho, 'n' focinho, 't' cauda (abana).

// Pequeno (schnauzer miniatura): pela altura do joelho do dono.
static const char *const DOG_RUN[2][5] = {
    {".....#.", "t...##n", "t####g.", ".####..", ".#..#.."},
    {".....#.", "t...##n", "t####g.", ".####..", "#..#..#"},
};
static const char *const DOG_SIT[5] = {"..#..", "..##n", "t.##g", "####.", ".#.#."};
static const char *const DOG_SLEEP[3] = {"#..#..", "#.###.", "#####g"};

#define PET_P 9.0f       // s por cada lançamento
#define PET_V 48.0f      // px/s a correr
#define PET_GO 1.2f      // o cão parte
#define PET_THROW 0.8f   // a bola sai da mão
#define PET_LAND 1.9f    // e cai

static uint32_t mixc(uint32_t a, uint32_t b, int k)   // k 0..100 (0 = a)
{
    int r = (int)((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * k) / 100;
    int g = (int)((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * k) / 100;
    int bb = (int)(a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * k) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bb;
}

// Sprite com os pés em y e o canto esquerdo em x; left = virado à esquerda (espelhado).
// Contorno escuro meio transparente, para se ver sobre a relva clara ou a neve.
static void dog_sprite(const char *const *rows, int nr, int x, int y, bool left, int k, bool wag)
{
    const int w = (int)strlen(rows[0]);
    const int top = y - nr + 1;
    uint32_t fur = (mixc(0xA8B0CC, 0xF4F4EE, k)), beard = (mixc(0x737B9A, 0xC2C6CE, k));   // branco ao luar
    uint32_t dark = (0x1E1E28);
    bool on[8][12] = {{0}};
    for (int j = 0; j < nr; j++)
        for (int i = 0; i < w; i++) {
            char ch = rows[j][i];
            if (ch == '.') continue;
            int jj = (ch == 't' && wag) ? j - 1 : j;
            on[jj + 1][(left ? w - 1 - i : i) + 1] = true;
        }
    for (int j = 0; j < nr + 1; j++)
        for (int i = 0; i < w + 2; i++) {
            if (on[j][i]) continue;
            bool n = (j > 0 && on[j - 1][i]) || on[j + 1][i] || (i > 0 && on[j][i - 1]) || (i < 11 && on[j][i + 1]);
            if (n) blend(x + i - 1, top + j - 1, 0x262A3A, 130);
        }
    for (int j = 0; j < nr; j++)
        for (int i = 0; i < w; i++) {
            char ch = rows[j][i];
            if (ch == '.') continue;
            int jj = (ch == 't' && wag) ? j - 1 : j;
            put(x + (left ? w - 1 - i : i), top + jj, ch == 'g' ? beard : (ch == 'e' || ch == 'n') ? dark : fur);
        }
}

// O dono, virado à esquerda, com os pés em (x.., gy). arm: ângulo do braço a partir de "para baixo"
// (positivo = para a frente). Devolve a posição da mão.
static void owner(int x, int gy, float arm, int k, int *hx, int *hy)
{
    uint32_t hair = (mixc(0x1A120E, 0x3C281E, k)), skin = (mixc(0x6A5650, 0xE6BEA0, k));
    uint32_t shirt = (mixc(0x1E2E48, 0x3A6EA8, k)), pants = (mixc(0x161A26, 0x2A2F45, k));
    // contorno suave
    for (int j = gy - 9; j <= gy + 1; j++)
        for (int i = x - 2; i <= x + 3; i++) {
            bool body = (j >= gy - 8 && j <= gy - 7 && (i == x || i == x + 1)) ||
                        (j >= gy - 6 && j <= gy - 3 && i >= x - 1 && i <= x + 2) || (j >= gy - 2 && j <= gy && (i == x || i == x + 1));
            if (!body) {
                bool n = false;
                for (int d = 0; d < 4 && !n; d++) {
                    int ii = i + (d == 0) - (d == 1), jj = j + (d == 2) - (d == 3);
                    n = (jj >= gy - 8 && jj <= gy - 7 && (ii == x || ii == x + 1)) ||
                        (jj >= gy - 6 && jj <= gy - 3 && ii >= x - 1 && ii <= x + 2) ||
                        (jj >= gy - 2 && jj <= gy && (ii == x || ii == x + 1));
                }
                if (n) blend(i, j, 0x262A3A, 100);
            }
        }
    rect(x, gy - 8, 2, 1, hair);
    rect(x, gy - 7, 2, 1, skin);
    put(x + 1, gy - 7, hair);   // nuca
    rect(x - 1, gy - 6, 4, 4, shirt);
    rect(x, gy - 2, 1, 3, pants);
    rect(x + 1, gy - 2, 1, 3, pants);
    float sx = x + 0.5f, sy = gy - 5.5f, dx = -sinf(arm), dy = cosf(arm);
    for (int s = 1; s <= 3; s++) put((int)lroundf(sx + dx * s), (int)lroundf(sy + dy * s), s == 3 ? skin : shirt);
    *hx = (int)lroundf(sx + dx * 4);
    *hy = (int)lroundf(sy + dy * 4);
}

static void toy(bool stick, float x, float y, float ang, int k)
{
    if (stick) {
        uint32_t c = (mixc(0x2E2012, 0x8A5A2C, k));
        float cx = cosf(ang), cy = sinf(ang);
        for (int s = -2; s <= 2; s++) put((int)lroundf(x + cx * s * 0.9f), (int)lroundf(y + cy * s * 0.9f), c);
    } else {
        uint32_t c = (mixc(0x501818, 0xE03A3A, k)), hi = (mixc(0x704040, 0xFF9A8A, k));
        int bx = (int)lroundf(x), by = (int)lroundf(y);
        rect(bx, by, 2, 2, c);
        put(bx, by, hi);
    }
}

static float lerpf(float a, float b, float s) { return a + (b - a) * s; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void pet_draw(const clock3_t *st, float t)
{
    const int k = st->pet_l < 0 ? 0 : st->pet_l > 100 ? 100 : st->pet_l;
    const int gy = st->pet_y + 3;   // pés na relva, à frente da casa
    if (st->pet == 0) {             // a dormir ao lado da casa
        int x = st->pet_x + 16;
        dog_sprite(DOG_SLEEP, 3, x, gy, false, k, false);
        float ph = fmodf(t, 3.0f) / 3.0f;   // um "z" que sobe e se desvanece
        int zx = x + 5 + (int)(ph * 4), zy = gy - 7 - (int)(ph * 9);
        int a = (int)(230 * (1 - ph));
        uint32_t zc = mixc(0xC8D0EA, 0xFFFFFF, k);
        for (int i = 0; i < 4; i++) { blend(zx + i, zy, zc, a); blend(zx + i, zy + 3, zc, a); }
        blend(zx + 2, zy + 1, zc, a);
        blend(zx + 1, zy + 2, zc, a);
        return;
    }
    const int n = (int)(t / PET_P);
    const float u = fmodf(t, PET_P);
    const bool stick = n % 3 == 2;   // de vez em quando uma vareta em vez da bola
    const int ox = st->pet_x - 7;    // dono à esquerda da casa
    const int xs = ox - 8;           // onde o cão se senta (5 px de largura)
    int xl = ox - 36 - (n * 37) % 70;   // onde a bola cai
    if (xl < 8) xl = 8;
    const float xb = stick ? xl : xl - 3;   // onde fica no chão (a bola ainda salta um pouco)
    const float run0 = xs - 2, run1 = xb + (stick ? 3 : 2);   // x do cão (sprite de 7) ao partir e ao chegar
    const float d = run0 - run1, tr = d / PET_V;
    const float ta = PET_GO + tr, tp = ta + 0.35f, tb = tp + tr;

    // braço: em repouso, puxa atrás, atira, e volta devagar
    float arm;
    if (u < 0.3f) arm = 0.3f;
    else if (u < 0.7f) arm = lerpf(0.3f, -2.5f, (u - 0.3f) / 0.4f);
    else if (u < 0.85f) arm = lerpf(-2.5f, 2.2f, (u - 0.7f) / 0.15f);
    else if (u < 1.6f) arm = 2.2f;
    else arm = lerpf(2.2f, 0.3f, clamp01((u - 1.6f) / 0.5f));
    int hx, hy;
    owner(ox, gy, arm, k, &hx, &hy);

    // o cão
    const int fr = ((int)(t * 9)) & 1;
    float dx;
    if (u < PET_GO) {
        dog_sprite(DOG_SIT, 5, xs, gy, true, k, u > PET_THROW && fr);   // abana a cauda quando a bola sai
    } else if (u < ta) {
        dx = run0 - (u - PET_GO) * PET_V;
        dog_sprite(DOG_RUN[fr], 5, (int)dx, gy - (fr ? 1 : 0), true, k, false);
    } else if (u < tp) {
        dx = run1;
        dog_sprite(DOG_RUN[0], 5, (int)dx, gy, true, k, false);
        if (u > ta + 0.15f) toy(stick, stick ? dx - 1 : dx - 2, gy - 3, 0, k);   // já na boca
    } else if (u < tb) {
        dx = run1 + (u - tp) * PET_V;
        dog_sprite(DOG_RUN[fr], 5, (int)dx, gy - (fr ? 1 : 0), false, k, false);
        int mx = (int)dx + 7, my = gy - 3;
        toy(stick, stick ? mx : mx - 1, stick ? my : my - 1, 0, k);
    } else {
        float h = u < tb + 0.4f ? sinf((u - tb) / 0.4f * 3.14159f) * 3 : 0;   // pulinho de alegria
        dog_sprite(DOG_SIT, 5, xs, gy - (int)h, true, k, fr);
    }

    // a bola (ou a vareta)
    if (u < PET_THROW) {
        toy(stick, hx - 1, hy - 1, 1.2f, k);
    } else if (u < PET_LAND) {
        float s = (u - PET_THROW) / (PET_LAND - PET_THROW);
        float x = lerpf(hx - 1, xl, s), y = lerpf(hy - 1, gy - 1, s) - 22 * 4 * s * (1 - s);
        toy(stick, x, y, u * 14, k);
    } else if (u < ta + 0.15f) {
        if (!stick && u < PET_LAND + 0.3f) {
            float s = (u - PET_LAND) / 0.3f;
            toy(false, lerpf(xl, xb, s), gy - 1 - sinf(s * 3.14159f) * 3, 0, k);
        } else {
            toy(stick, xb, stick ? gy : gy - 1, 0, k);
        }
    } else if (u >= tp && u < tb) {
        // na boca (desenhada com o cão)
    } else if (u >= tb && u < PET_P - 0.5f) {
        toy(stick, ox - 3, stick ? gy : gy - 1, 0, k);   // largada aos pés do dono
    } else if (u >= tb) {
        toy(stick, hx - 1, hy - 1, 1.2f, k);             // o dono apanha-a para o próximo lançamento
    }
}


// meia resolução -> ecrã (cada píxel vira 2x2)
static void up2(uint32_t *dst, int w, int h, int stride)
{
    for (int y = 0; y < LO && y * 2 + 1 < h; y++) {
        const uint32_t *src = &s_lo[y * LO];
        uint32_t *d0 = &dst[(y * 2) * stride], *d1 = d0 + stride;
        for (int x = 0; x < LO && x * 2 + 1 < w; x++) {
            uint32_t v = src[x];
            d0[x * 2] = d0[x * 2 + 1] = d1[x * 2] = d1[x * 2 + 1] = v;
        }
    }
}

static void wx_icon(int x, int y, int sc, const clock3_t *c)
{
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            int k = c->icon[j * 16 + i];
            if (k > 0 && k < 8) rect(x + i * sc, y + j * sc, sc, sc, c->icon_pal[k]);
        }
}

// hora (os dois pontos a piscar) e data; x < 0 = ao centro
static void time_block(int x, int y, int sc, float t, const clock3_t *c, uint32_t ink, uint32_t dim, uint32_t out)
{
    int tw = pfont_width(c->time, sc);
    if (x < 0) x = BW / 2 - tw / 2;
    int x0 = x;
    char one[2] = {0, 0};
    uint32_t *buf = B;
    int w = BW, h = BH, st = BS;
    for (const char *p = c->time; *p; p++) {
        one[0] = *p;
        pfont_draw(buf, w, h, st, one, x, y, sc, *p == ':' && (int)(t * 2) % 2 ? dim : ink, out, -1);
        x += pfont_width(one, sc) + sc;
    }
    int ds = sc / 3 < 2 ? 2 : sc / 3;
    int dx = x0 + (tw - pfont_width(c->date, ds)) / 2;
    pfont_draw(buf, w, h, st, c->date, dx, y + 7 * sc + sc + 2, ds, mix(ink, 0x8090A0, 90), out, -1);
    B = buf; BW = w; BH = h; BS = st;
}

// previsão no canto direito (em baixo, ou em cima com top): ícone, temperatura e "máx/mín chuva"
static void wx_block(const clock3_t *c, uint32_t out, bool top)
{
    if (!c->has_wx) return;
    uint32_t *buf = B;
    int w = BW, h = BH, st = BS;
    char tt[12], mm[32];
    snprintf(tt, sizeof(tt), "%d\xC2\xB0", c->temp);
    snprintf(mm, sizeof(mm), "%d/%d %d%%", c->tmax, c->tmin, c->rain);
    int tw = pfont_width(tt, 4), mw = pfont_width(mm, 2);
    int rw = tw > mw ? tw : mw, right = w - 30, y = top ? 30 : h - 82;
    pfont_draw(buf, w, h, st, tt, right - rw, y, 4, 0xFFFFFF, out, -1);
    pfont_draw(buf, w, h, st, mm, right - rw, y + 38, 2, 0xA8D8FF, out, -1);
    B = buf; BW = w; BH = h; BS = st;
    if (c->has_icon) wx_icon(right - rw - 58, y - 2, 3, c);
}

// ---- temporizador e alarme: zonas de toque (também para o desenho)
typedef struct { int x, y, w, h; } zrect_t;
static zrect_t zone_rect(int z)
{
    int py = z >= CZ_AH_UP ? 236 : 0;   // painel de baixo: alarme
    switch (z) {
    case CZ_TMIN_UP: case CZ_AH_UP: return (zrect_t){54, py + 54, 100, 40};
    case CZ_TMIN_DN: case CZ_AH_DN: return (zrect_t){54, py + 168, 100, 40};
    case CZ_TSEC_UP: case CZ_AM_UP: return (zrect_t){184, py + 54, 100, 40};
    case CZ_TSEC_DN: case CZ_AM_DN: return (zrect_t){184, py + 168, 100, 40};
    case CZ_TGO: return (zrect_t){322, 70, 120, 56};
    case CZ_TRESET: return (zrect_t){322, 140, 120, 56};
    case CZ_AON: return (zrect_t){322, 236 + 100, 120, 64};
    }
    return (zrect_t){0, 0, 0, 0};
}

int anim_clock_zone(int face, int x, int y)
{
    if (face != CLK_FACE_TIMER) return CZ_NONE;
    for (int z = CZ_TMIN_UP; z <= CZ_AON; z++) {
        zrect_t r = zone_rect(z);
        if (x >= r.x - 6 && x < r.x + r.w + 6 && y >= r.y - 6 && y < r.y + r.h + 6) return z;
    }
    return CZ_NONE;
}

static void arrow(int z, bool up, uint32_t c, bool on)
{
    zrect_t r = zone_rect(z);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    uint32_t col = on ? c : 0x2A3444;
    for (int k = 0; k < 12; k++) rect(cx - k * 2, cy + (up ? k - 6 : 6 - k), k * 4 + 1, 2, col);
}

static void button(int z, uint32_t c, bool filled)
{
    zrect_t r = zone_rect(z);
    rect(r.x, r.y, r.w, r.h, filled ? mix(0x0A0F1C, c, 70) : 0x101828);
    rect(r.x, r.y, r.w, 2, c);
    rect(r.x, r.y + r.h - 2, r.w, 2, c);
    rect(r.x, r.y, 2, r.h, c);
    rect(r.x + r.w - 2, r.y, 2, r.h, c);
}

// mm:ss / hh:mm: cada par centrado nas suas setas e o ":" a meio, com folga dos dois lados
#define PAIR_SC 8
static void pair2(int v, int zone, int y, uint32_t c)
{
    char b[4];
    snprintf(b, sizeof(b), "%02d", v < 0 ? 0 : v > 99 ? 99 : v);
    zrect_t r = zone_rect(zone);
    pfont_draw(B, BW, BH, BS, b, r.x + r.w / 2 - pfont_width(b, PAIR_SC) / 2, y, PAIR_SC, c, 0, -1);
}

static void colon2(int za, int zb, int y, uint32_t c)
{
    zrect_t a = zone_rect(za), b = zone_rect(zb);
    int mid = (a.x + a.w / 2 + b.x + b.w / 2) / 2;
    pfont_draw(B, BW, BH, BS, ":", mid - pfont_width(":", PAIR_SC) / 2, y, PAIR_SC, c, 0, -1);
}

static void face_timer(const clock3_t *c, float t)
{
    const uint32_t CY = 0x5EF2FF, AM = 0xFFB000;
    uint32_t *buf = B;
    int w = BW, h = BH, st = BS;
    fill(0x05080F);
    for (int y = 4; y < h; y += 12)
        for (int x = 4; x < w; x += 12) put(x, y, 0x101A2A);
    rect(30, 232, w - 60, 1, 0x22304A);
    char s[40];
    // temporizador
    bool run = c->tend > 0, paused = !run && c->tleft > 0, edit = !run && !paused;
    int left = run ? (int)(c->tend - c->now) : paused ? c->tleft : c->tset;
    if (left < 0) left = 0;
    pfont_draw(buf, w, h, st, "TEMPORIZADOR", 54, 24, 2, CY, 0, -1);
    arrow(CZ_TMIN_UP, true, CY, edit);
    arrow(CZ_TMIN_DN, false, CY, edit);
    arrow(CZ_TSEC_UP, true, CY, edit);
    arrow(CZ_TSEC_DN, false, CY, edit);
    uint32_t dc = paused && (c->now % 2) ? 0x2E5A66 : run ? CY : 0xFFFFFF;
    pair2(left / 60, CZ_TMIN_UP, 104, dc);
    colon2(CZ_TMIN_UP, CZ_TSEC_UP, 104, (run && c->now % 2) ? 0x18233A : dc);
    pair2(left % 60, CZ_TSEC_UP, 104, dc);
    B = buf; BW = w; BH = h; BS = st;
    button(CZ_TGO, CY, run);
    zrect_t g = zone_rect(CZ_TGO);
    int gx = g.x + g.w / 2, gy = g.y + g.h / 2;
    if (run) {
        rect(gx - 11, gy - 12, 8, 24, CY);
        rect(gx + 3, gy - 12, 8, 24, CY);
    } else {
        for (int k = 0; k < 14; k++) rect(gx - 8 + k, gy - 13 + k, 1, 27 - 2 * k, CY);
    }
    button(CZ_TRESET, CY, false);
    zrect_t rr = zone_rect(CZ_TRESET);
    rect(rr.x + rr.w / 2 - 11, rr.y + rr.h / 2 - 11, 22, 22, CY);
    if (run) {
        time_t endt = (time_t)c->tend;
        struct tm et;
        gmtime_r(&endt, &et);
        snprintf(s, sizeof(s), "TERMINA %02d:%02d", et.tm_hour, et.tm_min);
    } else {
        snprintf(s, sizeof(s), "%s", paused ? "EM PAUSA" : c->tset ? "PRONTO" : "ESCOLHA O TEMPO");
    }
    pfont_draw(buf, w, h, st, s, 60, 214, 2, run ? CY : 0x8B98A5, 0, -1);
    B = buf; BW = w; BH = h; BS = st;
    // alarme
    pfont_draw(buf, w, h, st, "ALARME", 54, 260, 2, AM, 0, -1);
    arrow(CZ_AH_UP, true, AM, true);
    arrow(CZ_AH_DN, false, AM, true);
    arrow(CZ_AM_UP, true, AM, true);
    arrow(CZ_AM_DN, false, AM, true);
    uint32_t ac = c->alon ? AM : 0x9A8A6A;
    pair2(c->alh, CZ_AH_UP, 340, ac);
    colon2(CZ_AH_UP, CZ_AM_UP, 340, ac);
    pair2(c->alm, CZ_AM_UP, 340, ac);
    B = buf; BW = w; BH = h; BS = st;
    button(CZ_AON, AM, c->alon);
    zrect_t ab = zone_rect(CZ_AON);
    const char *al = c->alon ? "DESLIGAR" : "LIGAR";
    pfont_draw(buf, w, h, st, al, ab.x + (ab.w - pfont_width(al, 2)) / 2, ab.y + ab.h / 2 - 7, 2, c->alon ? 0xFFFFFF : AM, 0, -1);
    B = buf; BW = w; BH = h; BS = st;
    if (c->alon) {
        struct tm nt;
        time_t nn = (time_t)c->now;
        gmtime_r(&nn, &nt);
        int d = (c->alh * 60 + c->alm - (nt.tm_hour * 60 + nt.tm_min) + 1440) % 1440;
        if (d == 0) d = 1440;
        snprintf(s, sizeof(s), "TOCA EM %dH%02d", d / 60, d % 60);
    } else {
        snprintf(s, sizeof(s), "DESLIGADO");
    }
    pfont_draw(buf, w, h, st, s, 60, 450, 2, c->alon ? AM : 0x8B98A5, 0, -1);
    B = buf; BW = w; BH = h; BS = st;
    (void)t;
}

const char *anim_clock_name(int face)
{
    static const char *N[ANIM_CLOCK_FACES] = {"PAISAGEM", "AURORA", "CUBO", "TEMPORIZADOR E ALARME"};
    return N[((face % ANIM_CLOCK_FACES) + ANIM_CLOCK_FACES) % ANIM_CLOCK_FACES];
}

void anim_clock_draw(int face, uint32_t *buf, int w, int h, int stride, float t, const clock3_t *c)
{
    face = ((face % ANIM_CLOCK_FACES) + ANIM_CLOCK_FACES) % ANIM_CLOCK_FACES;
    B = buf; BW = w; BH = h; BS = stride;
    if (face == CLK_FACE_TIMER) {
        face_timer(c, t);
        return;
    }
    if (face == 2) {   // o cubo a rodar, a hora às cores, a bateria e o tempo
        char wx[40] = "";
        if (c->has_wx) snprintf(wx, sizeof(wx), "%d\xC2\xB0  %d/%d  CHUVA %d%%", c->temp, c->tmax, c->tmin, c->rain);
        away_t a = {.time = c->time, .date = c->date, .bat = c->bat, .chg = c->chg, .pending = -1, .sd = true,
                    .status = wx, .shift = c->shift};
        anim_away_draw(buf, w, h, stride, t, &a);
        return;
    }
    if (!s_lo) {
        s_lo = heap_caps_malloc(LO * LO * 4, MALLOC_CAP_SPIRAM);
        if (!s_lo) return;
        aurora_init();
    }
    B = s_lo; BW = LO; BH = LO; BS = LO;
    if (face == 0 && c->scene) {   // paisagem: a cena do dashd, a chuva/neve e o cão
        const bgimg_t *sc = c->scene;
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u | sc->pal[i];
        for (int y = 0; y < LO; y++)
            for (int x = 0; x < LO; x++)
                s_lo[y * LO + x] = (x < sc->w && y < sc->h) ? pal[bg_px(sc, x, y)] : 0xFF101830;
        if (c->pet >= 0) pet_draw(c, t);
        particles(c->anim, c->night, t);
    } else if (face == 0) {   // a cena ainda não chegou
        fill(0x101830);
    } else {
        aurora_scene(t);
    }
    up2(buf, w, h, stride);
    B = buf; BW = w; BH = h; BS = stride;
    int dx = (c->shift % 3 - 1) * 4, dy = (c->shift / 3 - 1) * 4;   // desloca-se um pouco a cada minuto (AMOLED)
    if (face == 0) time_block(28 + dx, 26 + dy, 7, t, c, 0xFFFFFF, 0x6A7488, 0x0E1020);
    else time_block(-1, 30 + dy, 8, t, c, 0xEFFFF8, 0x2E6A60, 0x07161C);
    wx_block(c, face == 0 ? 0x0E1020 : 0x07161C, face == 0);   // na paisagem, em cima (em baixo está a casa)
}

// ------------------------------------------------------------------------------------------ Pomodoro
// Ecrã do método Pomodoro: anel com o tempo que falta, a fase (foco, pausa, pausa longa), os quatro
// tomates do ciclo e dois botões. 480x480 na vertical de quem olha (o cubo está deitado de lado).

static const char *POMO_TOMATO[10] = {   // 10x10: v = folha, r = tomate, h = brilho
    "...vv.v...",
    "..vvvvv...",
    "..rrvrrr..",
    ".rrrrrrrr.",
    "rrhhrrrrrr",
    "rrhrrrrrrr",
    "rrrrrrrrrr",
    ".rrrrrrrr.",
    "..rrrrrr..",
    "...rrrr...",
};

static void pomo_tomato(int x, int y, int sc, bool on)
{
    for (int j = 0; j < 10; j++)
        for (int i = 0; i < 10; i++) {
            char k = POMO_TOMATO[j][i];
            if (k == '.') continue;
            uint32_t c = k == 'v' ? 0x3FBF4A : k == 'h' ? 0xFF9A8A : 0xE8453C;
            if (!on) c = k == 'v' ? 0x23402A : 0x3A2026;
            rect(x + i * sc, y + j * sc, sc, sc, c);
        }
}

static const struct { int x, y, w, h; } PZ_RECT[] = {
    [PZ_RESET] = {40, 404, 180, 52},
    [PZ_SKIP] = {260, 404, 180, 52},
};

int anim_pomo_zone(int x, int y)
{
    for (int z = PZ_RESET; z <= PZ_SKIP; z++)
        if (x >= PZ_RECT[z].x - 8 && x < PZ_RECT[z].x + PZ_RECT[z].w + 8 && y >= PZ_RECT[z].y - 8 &&
            y < PZ_RECT[z].y + PZ_RECT[z].h + 8)
            return z;
    return PZ_GO;   // o resto do ecrã: iniciar ou pausar
}

void anim_pomo_draw(uint32_t *buf, int w, int h, int stride, float t, const pomo_t *p)
{
    static const uint32_t COL[] = {0xE8453C, 0x3FBF6A, 0x4A9BFF};
    static const char *NAME[] = {"FOCO", "PAUSA", "PAUSA LONGA"};
    B = buf; BW = w; BH = h; BS = stride;
    int ph = p->phase < 0 || p->phase > 2 ? 0 : p->phase;
    uint32_t col = COL[ph], bg = mix(0x06080C, col, 14);
    fill(bg);
    // cabeçalho: tomate, fase, hora e focos de hoje
    pomo_tomato(28, 22, 4, true);
    pfont_draw(buf, w, h, stride, NAME[ph], 80, 30, 4, col, 0, -1);
    B = buf; BW = w; BH = h; BS = stride;
    if (p->clock) {
        pfont_draw(buf, w, h, stride, p->clock, w - 28 - pfont_width(p->clock, 3), 24, 3, 0xC8D0DA, 0, -1);
        B = buf; BW = w; BH = h; BS = stride;
    }
    char s[32];
    snprintf(s, sizeof(s), "HOJE %d", p->today);
    pfont_draw(buf, w, h, stride, s, w - 28 - pfont_width(s, 2), 54, 2, 0x7D8794, 0, -1);
    B = buf; BW = w; BH = h; BS = stride;
    // anel: a cor é o que falta, a contar do topo no sentido dos ponteiros
    const int cx = w / 2, cy = 212, ro = 140, ri = 121;
    float frac = p->total > 0 ? (float)p->left / p->total : 0;
    uint32_t track = mix(bg, 0xFFFFFF, 22);
    for (int y = cy - ro; y <= cy + ro; y++)
        for (int x = cx - ro; x <= cx + ro; x++) {
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 > ro * ro || d2 < ri * ri) continue;
            float a = atan2f((float)dx, (float)-dy);   // 0 no topo, a crescer no sentido dos ponteiros
            if (a < 0) a += 6.2831853f;
            put(x, y, a / 6.2831853f <= frac ? col : track);
        }
    if (p->run && frac > 0) {   // ponto a pulsar na ponta do anel
        float a = frac * 6.2831853f, rm = (ro + ri) / 2.0f;
        int px = cx + (int)lroundf(sinf(a) * rm), py = cy - (int)lroundf(cosf(a) * rm);
        int r = 13 + (int)(3 * sinf(t * 4));
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++)
                if (x * x + y * y <= r * r) put(px + x, py + y, mix(col, 0xFFFFFF, 110));
    }
    // tempo que falta
    snprintf(s, sizeof(s), "%02d:%02d", p->left / 60, p->left % 60);
    uint32_t tc = !p->run && p->left < p->total && ((int)(t * 2) & 1) ? mix(bg, 0xFFFFFF, 90) : 0xFFFFFF;
    pfont_draw(buf, w, h, stride, s, cx - pfont_width(s, 8) / 2, cy - 36, 8, tc, 0, -1);
    B = buf; BW = w; BH = h; BS = stride;
    const char *st = p->run ? (ph == POMO_FOCUS ? "CONCENTRE-SE" : "DESCANSE")
                   : p->left < p->total ? "EM PAUSA" : "TOQUE PARA INICIAR";
    pfont_draw(buf, w, h, stride, st, cx - pfont_width(st, 2) / 2, cy + 36, 2, p->run ? col : 0xAEB6C0, 0, -1);
    B = buf; BW = w; BH = h; BS = stride;
    // os quatro focos do ciclo
    for (int i = 0; i < 4; i++) pomo_tomato(cx - 75 + i * 40, 364, 3, i < p->done);
    // botões
    static const char *BL[] = {[PZ_RESET] = "REINICIAR", [PZ_SKIP] = "PULAR"};
    for (int z = PZ_RESET; z <= PZ_SKIP; z++) {
        int x = PZ_RECT[z].x, y = PZ_RECT[z].y, bw = PZ_RECT[z].w, bh = PZ_RECT[z].h;
        uint32_t c = mix(bg, col, 150);
        rect(x, y, bw, bh, mix(bg, 0x000000, 60));
        rect(x, y, bw, 2, c);
        rect(x, y + bh - 2, bw, 2, c);
        rect(x, y, 2, bh, c);
        rect(x + bw - 2, y, 2, bh, c);
        pfont_draw(buf, w, h, stride, BL[z], x + (bw - pfont_width(BL[z], 2)) / 2, y + bh / 2 - 7, 2, 0xE6EAF0, 0, -1);
        B = buf; BW = w; BH = h; BS = stride;
    }
}

// ------------------------------------------------------------------------------------- arranque (8 bits)
// Pixel art de jogo de 8 bits, como a paisagem do relógio do DashDeck: desenhado a 240 x h/2 e ampliado
// 2x. Noite com estrelas, lua e montanhas. (1) As três camadas do cubo caem uma a uma e assentam com
// ressalto; (2) as letras de DASHDECK caem uma a uma; (3) o cubo salta para o lugar do expoente, a
// encolher aos degraus; (4) desfaz-se num estouro de pixels e o ³ aparece linha a linha, com as cores do
// cubo. O nome acaba no centro do ecrã. Depois: estrelas a piscar, um brilho que corre pelo nome e um
// cintilar no ³.

#define BL_SC 4       // bloco das letras (píxeis baixos)
#define BL_SUP 4      // bloco do ³ (o glifo tem 5 linhas: continua a ler-se como expoente)
#define BL_L 20       // aresta do cubo no início
#define T_LETTERS 1.35f
#define T_JUMP 2.45f
#define T_LAND 3.15f
#define T_DONE 3.6f

static int s_bh, s_bcx, s_bcy, s_bsup_x, s_bsup_y, s_bsup_top, s_bname_y;
static int16_t s_bc[260][3];
static int s_bn;
static int16_t s_bstar[70][3];

static float bounce(float t)
{
    t = clampf(t);
    if (t < 1 / 2.75f) return 7.5625f * t * t;
    if (t < 2 / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
    if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
    t -= 2.625f / 2.75f;
    return 7.5625f * t * t + 0.984375f;
}

// cubo isométrico 2:1 em pixel art, centrado em (cx, cy), aresta L; só a fatia z0..z1 (0..1, para as
// camadas que caem); ps = tamanho do pixel. Cada face: uma cor da marca, 3x3 quadrados com contorno e um brilho no canto.
static void cube8(int cx, int cy, int L, float z0, float z1, int ps)
{
    if (L < 2) return;
    for (int y = -L - 1; y <= L + 1; y++)
        for (int x = -L; x < L; x++) {
            float sx = x + 0.5f, sy = y + 0.5f;
            int face = -1;
            float u = 0, v = 0;
            float a = sx / L, b = 2 * (sy + z1 * L) / L;
            float X = (a + b) / 2, Y = (b - a) / 2;
            if (X >= 0 && X <= 1 && Y >= 0 && Y <= 1) {
                face = 0; u = X; v = Y;
            } else if (sx >= 0) {
                float Yr = 1 - sx / L, Z = ((1 + Yr) * L / 2 - sy) / L;
                if (Yr >= 0 && Yr <= 1 && Z >= z0 && Z <= z1) { face = 1; u = Yr; v = 1 - Z; }
            } else {
                float Xl = 1 + sx / L, Z = ((Xl + 1) * L / 2 - sy) / L;
                if (Xl >= 0 && Xl <= 1 && Z >= z0 && Z <= z1) { face = 2; u = Xl; v = 1 - Z; }
            }
            if (face < 0) continue;
            int i = (int)(u * 3), j = (int)(v * 3);
            i = i > 2 ? 2 : i;
            j = j > 2 ? 2 : j;
            float fu = u * 3 - i, fv = v * 3 - j, bw = 1.0f / (L / 3.0f);
            uint32_t base = CUBE8[face];
            uint32_t c;
            if (u < 0.03f || u > 0.97f || v < 0.03f || v > 0.97f) c = 0x0A0818;            // contorno
            else if (fu < bw || fv < bw) c = mix(base, 0x0A0818, 150);                     // juntas
            else if (fu < bw * 2 && fv < bw * 2 && L >= 12) c = mix(base, 0xFFFFFF, 150);  // 1 pixel de brilho
            else c = base;
            if (face == 2) c = mix(c, 0x0A0818, 70);    // esquerda: meia sombra
            if (face == 1) c = mix(c, 0x0A0818, 120);   // direita: sombra
            if (ps == 1) put(cx + x, cy + y, c);
            else rect(cx + x * ps, cy + y * ps, ps, ps, c);
        }
}

// estrela cadente: cabeça branca de 2x2 e rasto que passa a âmbar e se apaga; k = segundos desde a partida
static void meteor8(float k, int x0, int y0, float dx, float dy)
{
    if (k < 0 || k > 0.9f) return;
    float p = k / 0.9f;
    float hx = x0 + dx * p * 190, hy = y0 + dy * p * 190;
    int fade = p > 0.75f ? (int)((1 - p) / 0.25f * 256) : 256;   // apaga-se no fim
    for (int i = 22; i >= 0; i--) {
        int px = (int)(hx - dx * i * 1.4f), py = (int)(hy - dy * i * 1.4f);
        uint32_t c = i < 3 ? 0xFFFFFF : i < 9 ? 0xFFE9A8 : 0xFF9A5A;
        blend(px, py, c, (256 - i * 11) * fade / 256);
    }
    blend((int)hx + 1, (int)hy, 0xFFFFFF, fade);
    blend((int)hx, (int)hy + 1, 0xFFFFFF, fade);
    blend((int)hx + 1, (int)hy + 1, 0xFFFFFF, fade);
}

static void sky8(float t)
{
    // céu em faixas (sem gradiente suave: 8 bits), lua e montanhas
    static const uint32_t SKY[] = {0x05061A, 0x080A24, 0x0C0E30, 0x12123C, 0x1A1648, 0x251A52};
    for (int y = 0; y < s_bh; y++) {
        int k = y * 6 / s_bh;
        uint32_t c = SKY[k > 5 ? 5 : k];
        uint32_t *row = &B[y * BS];
        for (int x = 0; x < BW; x++) row[x] = 0xFF000000u | c;
    }
    for (int i = 0; i < 70; i++) {
        int ph = ((int)(t * 4) + s_bstar[i][2]) % 12;
        if (ph == 0) continue;   // pisca
        uint32_t c = s_bstar[i][2] % 5 == 0 ? 0xFFE9A8 : 0xC8D4FF;
        put(s_bstar[i][0], s_bstar[i][1], ph < 3 ? mix(c, 0, 140) : c);
        if (s_bstar[i][2] % 11 == 0 && ph > 6) {   // as maiores em cruz
            put(s_bstar[i][0] - 1, s_bstar[i][1], mix(c, 0, 128));
            put(s_bstar[i][0] + 1, s_bstar[i][1], mix(c, 0, 128));
            put(s_bstar[i][0], s_bstar[i][1] - 1, mix(c, 0, 128));
            put(s_bstar[i][0], s_bstar[i][1] + 1, mix(c, 0, 128));
        }
    }
    // meteoros: um aos 0,7 s, outro aos 2,9 s; depois um a cada 6 s
    meteor8(t - 0.7f, 236, 6, -0.86f, 0.5f);
    meteor8(t - 2.9f, 170, 2, -0.78f, 0.62f);
    if (t > 5) {
        int n = (int)((t - 5) / 6);
        meteor8(fmodf(t - 5, 6), 150 + (n * 37) % 90, 2 + (n * 13) % 20, -0.82f, 0.57f);
    }
    int mx = 204, my = 26;   // lua com crateras
    for (int y = -8; y <= 8; y++)
        for (int x = -8; x <= 8; x++)
            if (x * x + y * y <= 64) put(mx + x, my + y, x + y > 6 ? 0xD8C890 : 0xFFF2C4);
    put(mx - 3, my - 2, 0xE2D49E); put(mx - 2, my - 2, 0xE2D49E); put(mx + 2, my + 3, 0xE2D49E);
    // montanhas: duas camadas em degraus
    for (int x = 0; x < BW; x++) {
        int h1 = (int)(26 + 14 * sinf(x * 0.035f) + 8 * sinf(x * 0.11f + 1));
        int h2 = (int)(12 + 6 * sinf(x * 0.06f + 2) + 4 * sinf(x * 0.19f));
        for (int y = s_bh - h1; y < s_bh; y++) put(x, y, y < s_bh - h1 + 2 ? 0x3A2A6A : 0x221846);
        for (int y = s_bh - h2; y < s_bh; y++) put(x, y, y < s_bh - h2 + 1 ? 0x2A1C50 : 0x130D2C);
    }
}

void anim_boot_start(int w, int h)
{
    if (!s_lo) s_lo = heap_caps_malloc(LO * LO * 4, MALLOC_CAP_SPIRAM);
    s_bh = h / 2 > LO ? LO : h / 2;
    BW = LO;
    BH = s_bh;
    s_seed = 7;
    for (int i = 0; i < 70; i++) {
        s_bstar[i][0] = (int16_t)(frand() * LO);
        s_bstar[i][1] = (int16_t)(frand() * (s_bh - 40));
        s_bstar[i][2] = (int16_t)(frand() * 60);
    }
    // o nome acaba no centro do ecrã (o canvas começa 56 px abaixo do topo: 28 em píxeis baixos)
    char f[16];
    fold(LOGO, f, sizeof(f));
    s_sup_fix = BL_SUP;
    int nw = pfont_width(LOGO, BL_SC);
    s_bname_y = (LO / 2 - 28) - 7 * BL_SC / 2;
    s_bn = cells(f, (LO - nw) / 2, s_bname_y, BL_SC, s_bc, 260);
    s_sup_fix = 0;
    int x0 = 999, x1 = -1, y0 = 999, y1 = -1;
    for (int i = 0; i < s_bn; i++)
        if (s_bc[i][2] & SUP) {
            if (s_bc[i][0] < x0) x0 = s_bc[i][0];
            if (s_bc[i][0] + BL_SUP > x1) x1 = s_bc[i][0] + BL_SUP;
            if (s_bc[i][1] < y0) y0 = s_bc[i][1];
            if (s_bc[i][1] + BL_SUP > y1) y1 = s_bc[i][1] + BL_SUP;
        }
    s_bsup_x = (x0 + x1) / 2;
    s_bsup_y = (y0 + y1) / 2;
    s_bsup_top = y0;
    s_bcx = LO / 2;
    s_bcy = s_bname_y - BL_L - 14;   // o cubo por cima do nome
}

void anim_boot_draw(uint32_t *buf, int stride, float t, float progress, const char *status, float tilt)
{
    (void)tilt;
    if (!s_lo) return;
    B = s_lo; BW = LO; BH = s_bh; BS = LO;
    sky8(t);
    // (1) as camadas do cubo caem; (3) depois salta para o expoente, a encolher aos degraus
    if (t < T_LAND) {
        int cx = s_bcx, cy = s_bcy, L = BL_L;
        if (t > T_JUMP) {
            float k = clampf((t - T_JUMP) / (T_LAND - T_JUMP));
            cx = s_bcx + (int)lroundf((s_bsup_x - s_bcx) * k);
            cy = s_bcy + (int)lroundf((s_bsup_y - s_bcy) * k - 34 * sinf(k * 3.14159f));   // arco do salto
            L = BL_L - ((int)(k * 4)) * 4;   // 20, 16, 12, 8: encolhe aos degraus
            for (int p = 1; p <= 4; p++) {   // rasto de faíscas
                float kp = k - p * 0.06f;
                if (kp < 0) break;
                int px = s_bcx + (int)((s_bsup_x - s_bcx) * kp);
                int py = s_bcy + (int)((s_bsup_y - s_bcy) * kp - 34 * sinf(kp * 3.14159f));
                put(px, py, CUBE8[p % 3]);
            }
            cube8(cx, cy, L, 0, 1, 1);
        } else {
            for (int k = 0; k < 3; k++) {
                float t0 = 0.2f + k * 0.35f;
                if (t < t0) break;
                float e = bounce((t - t0) / 0.55f);
                int dy = (int)lroundf(-(1 - e) * (cy + 40));
                // a camada k desenha-se sozinha, deslocada; as de baixo já assentaram
                cube8(cx, cy + dy, L, k / 3.0f, (k + 1) / 3.0f, 1);
            }
        }
    }
    // (4) estouro de pixels quando o cubo chega ao expoente
    float pk = (t - T_LAND) / 0.5f;
    if (pk > 0 && pk < 1) {
        for (int i = 0; i < 14; i++) {
            float a = i * 0.4488f, r = 3 + pk * 22;
            int px = s_bsup_x + (int)(cosf(a) * r), py = s_bsup_y + (int)(sinf(a) * r * 0.8f + pk * pk * 10);
            uint32_t c = CUBE8[i % 3];
            put(px, py, c);
            if (pk < 0.5f) put(px + 1, py, c);
        }
    }
    // (2) letras a cair; o ³ aparece linha a linha depois do estouro
    int sweep = t > T_DONE ? (int)fmodf((t - T_DONE) * 120, LO + 160) - 80 : -999;
    for (int pass = 0; pass < 2; pass++)   // 0 = sombra, 1 = cor
        for (int i = 0; i < s_bn; i++) {
            bool sup = s_bc[i][2] & SUP;
            int ci = s_bc[i][2] & 0xFF, x = s_bc[i][0], y = s_bc[i][1];
            int z = sup ? BL_SUP : BL_SC;
            uint32_t c;
            if (sup) {
                int row = (y - s_bsup_top) / BL_SUP;
                if (t < T_LAND + 0.05f + row * 0.07f) continue;
                c = CUBE8[((row + 1) / 2) % 3];
            } else {
                float t0 = T_LETTERS + ci * 0.11f;
                if (t < t0) continue;
                y -= (int)lroundf((1 - bounce((t - t0) / 0.6f)) * (y + 30));
                c = PAL[(ci * 2) % 8];
            }
            if (pass == 0) {
                rect(x + 1, y + 1, z, z, 0x05040E);
                continue;
            }
            int d = abs(x - sweep);
            if (d < 14) c = mix(c, 0xFFFFFF, (14 - d) * 14);
            rect(x, y, z, z, c);
        }
    // cintilar no ³ (estrela de 4 pontas a cada 1,4 s)
    if (t > T_DONE) {
        float ph = fmodf(t - T_DONE, 1.4f);
        if (ph < 0.35f) {
            int r = ph < 0.12f || ph > 0.24f ? 1 : 2, sx = s_bsup_x + 6, sy = s_bsup_top - 3;
            put(sx, sy, 0xFFFFFF);
            for (int k = 1; k <= r; k++) {
                put(sx - k, sy, 0xFFF2C4); put(sx + k, sy, 0xFFF2C4);
                put(sx, sy - k, 0xFFF2C4); put(sx, sy + k, 0xFFF2C4);
            }
        }
    }
    // barra de progresso e estado da ligação (também em píxeis grossos)
    int bw = 140, bx = (LO - bw) / 2, by = s_bh - 10;
    rect(bx - 1, by - 1, bw + 2, 5, 0x05040E);
    rect(bx, by, bw, 3, 0x2A2050);
    int fw = (int)(bw * clampf(progress));
    for (int x = 0; x < fw; x++) rect(bx + x, by, 1, 3, PAL[(x * 8 / bw) % 8]);
    if (status && *status) {
        int tw = pfont_width(status, 1);
        pfont_draw(s_lo, LO, s_bh, LO, status, (LO - tw) / 2, by - 12, 1, 0xC8D0E8, 0x05040E, -1);
    }
    up2(buf, LO * 2, s_bh * 2, stride);
}
