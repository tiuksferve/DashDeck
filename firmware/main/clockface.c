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

// Fundo escuro translúcido atrás de um bloco de texto, com as bordas esbatidas (F px), para o texto
// se ler sobre céu claro, neve ou nuvens sem tapar a paisagem. a = alfa no centro (0..256).
#define SCRIM_F 16
#define SCRIM_C 0x060A14
#define SCRIM_A 85    // opacidade no centro (0..256): mais alto = sombra mais escura
static void scrim(int x, int y, int w, int h, int a)
{
    const int F = SCRIM_F;
    for (int j = y - F; j < y + h + F; j++) {
        if (j < 0 || j >= BH) continue;
        int dy = j < y ? y - j : (j >= y + h ? j - (y + h - 1) : 0);
        for (int i = x - F; i < x + w + F; i++) {
            if (i < 0 || i >= BW) continue;
            int dx = i < x ? x - i : (i >= x + w ? i - (x + w - 1) : 0);
            int d = dx > dy ? dx + dy / 2 : dy + dx / 2;   // distância aproximada (cantos redondos)
            if (d >= F) continue;
            int k = F - d;                                   // 1..F
            blend(i, j, SCRIM_C, a * k * k / (F * F));       // queda suave
        }
    }
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

// Contorno de um "pixel" da fonte (bloco sc x sc): 1 px nas letras pequenas, 2 px a partir da escala 3,
// mais uma sombra projetada para baixo e para a direita (destaca os números sobre fundos claros).
static void outline_px(int x, int y, int sc, uint16_t out)
{
    int o = sc >= 3 ? 2 : 1, sh = sc >= 3 ? 2 : 1;
    rect(x - o, y - o, sc + 2 * o, sc + 2 * o, out);
    rect(x + sh, y + sh, sc + o, sc + o, out);
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
                        if (pass == 0) outline_px(cx + k * sc, y + r * sc, sc, (uint16_t)out);
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
                        if (pass == 0) outline_px(cx + k * sc, y + r * sc, sc, (uint16_t)out);
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

static void icon_raw(const uint8_t *px, const uint16_t *pal, int x, int y, int sc)
{
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            uint8_t v = px[j * 16 + i];
            if (v && v <= 7) rect(x + i * sc, y + j * sc, sc, sc, pal[v]);
        }
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

// ícone 16x16 ampliado para size x size px (tamanhos não inteiros, ex.: 24), com contorno de 1 px
static void icon_sz(const clock_state_t *st, int x, int y, int size, int out)
{
    if (!st->has_icon) return;
    for (int pass = out >= 0 ? 0 : 1; pass < 2; pass++)
        for (int dy = 0; dy < size; dy++)
            for (int dx = 0; dx < size; dx++) {
                uint8_t v = st->icon[(dy * 16 / size) * 16 + dx * 16 / size];
                if (!v || v > 7) continue;
                if (pass == 0) rect(x + dx - 1, y + dy - 1, 3, 3, (uint16_t)out);
                else put(x + dx, y + dy, st->icon_pal[v]);
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

// ---------- o cão junto à casa (paisagem) ----------
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
    uint16_t fur = c565(mixc(0xA8B0CC, 0xF4F4EE, k)), beard = c565(mixc(0x737B9A, 0xC2C6CE, k));   // branco ao luar
    uint16_t dark = c565(0x1E1E28);
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
    uint16_t hair = c565(mixc(0x1A120E, 0x3C281E, k)), skin = c565(mixc(0x6A5650, 0xE6BEA0, k));
    uint16_t shirt = c565(mixc(0x1E2E48, 0x3A6EA8, k)), pants = c565(mixc(0x161A26, 0x2A2F45, k));
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
        uint16_t c = c565(mixc(0x2E2012, 0x8A5A2C, k));
        float cx = cosf(ang), cy = sinf(ang);
        for (int s = -2; s <= 2; s++) put((int)lroundf(x + cx * s * 0.9f), (int)lroundf(y + cy * s * 0.9f), c);
    } else {
        uint16_t c = c565(mixc(0x501818, 0xE03A3A, k)), hi = c565(mixc(0x704040, 0xFF9A8A, k));
        int bx = (int)lroundf(x), by = (int)lroundf(y);
        rect(bx, by, 2, 2, c);
        put(bx, by, hi);
    }
}

static float lerpf(float a, float b, float s) { return a + (b - a) * s; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void pet_draw(const clock_state_t *st, float t)
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

void clockface_draw_pet(uint16_t *buf, int stride_px, int w, int h, const clock_state_t *st, uint32_t ms)
{
    B = buf;
    BW = w;
    BH = h;
    BS = stride_px;
    if (st->face == 0 && st->pet >= 0) pet_draw(st, (float)(ms % 3600000u) / 1000.0f);
}

bool clockface_pet_rect(const clock_state_t *st, int *x, int *y, int *w, int *h)
{
    if (st->face != 0 || st->pet != 1) return false;
    int x0 = st->pet_x - 7 - 36 - 70 - 8;
    if (x0 < 0) x0 = 0;
    *x = x0;
    *y = st->pet_y + 3 - 40;
    *w = st->pet_x - x0;
    *h = 46;
    return true;
}

// ---------- mostradores ----------

static void face_landscape(const clock_state_t *st, const struct tm *tm, float t)
{
    if (st->face == 0 && st->pet >= 0) pet_draw(st, t);
    particles(st, t);
    const uint16_t ink = c565(0xFFFFFF);
    const int out = c565(0x0E1020);
    const bool land = BW > BH;
    char buf[24];
    int x = 10;
    scrim(10, 10, st->has_time ? 110 : 80, st->has_time ? 36 : 22, SCRIM_A);   // hora e data
    const int wxx = land ? BW - 108 : 10, wxy = land ? 8 : 62;   // bloco do tempo (canto direito)
    if (st->has_wx) scrim(wxx + 2, wxy + 2, 96, 38, SCRIM_A);
    if (st->has_time) {
        // hora pequena (escala 3, como na previsão, com os segundos maiores): a paisagem fica à vista
        snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
        x += text5(buf, x, 10, 3, ink, out) + 3;
        if (tm->tm_sec % 2 == 0) text5(":", x, 10, 3, ink, out);
        x += 9;
        snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
        x += text5(buf, x, 10, 3, ink, out) + 4;
        snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
        text5(buf, x, 17, 2, c565(0xFFE9A8), out);
        snprintf(buf, sizeof(buf), "%s %d %s", DOW[tm->tm_wday % 7], tm->tm_mday, MON[tm->tm_mon % 12]);
        text5(buf, 11, 37, 1, ink, out);
    } else {
        text5("--:--", 10, 10, 3, ink, out);
    }
    if (!st->has_wx) return;
    const int ix = wxx, iy = wxy;
    icon_sz(st, ix + 2, iy + 2, 24, out);   // 24 px (era 32) e temperatura na escala 2 (era 3)
    snprintf(buf, sizeof(buf), "%d" DEG, st->temp);
    text5(buf, ix + 32, iy + 7, 2, ink, out);
    int xx = ix + 2, yy = iy + 30;
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

// Mostrador 2: previsão por horas sobre a costa. Hora em cima; em baixo, numa faixa escura, as próximas
// horas (hora, ícone, temperatura, chuva).
static void face_forecast(const clock_state_t *st, const struct tm *tm, float t)
{
    particles(st, t);
    const uint16_t ink = c565(0xFFFFFF);
    const int out = c565(0x0E1020);
    const bool land = BW > BH;
    char buf[24];
    int x = 8;
    scrim(8, 6, 100, 32, SCRIM_A);
    if (st->has_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        x += text5(buf, x, 6, 3, ink, out) + 4;
        snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
        text5(buf, x, 20, 1, c565(0xFFE9A8), out);
        snprintf(buf, sizeof(buf), "%s %d %s", DOW[tm->tm_wday % 7], tm->tm_mday, MON[tm->tm_mon % 12]);
        text5(buf, 8, 32, 1, ink, out);
    } else {
        text5("--:--", 8, 6, 3, ink, out);
    }
    if (!st->nh) {
        scrim(10, BH - 22, tw5("SEM PREVISAO", 2) - 4, 14, SCRIM_A);
        text5("SEM PREVISAO", 8, BH - 20, 2, ink, out);
        return;
    }
    // faixa: 6 colunas na horizontal; 3 x 2 na vertical
    const int cols = land ? 6 : 3, rows = land ? 1 : 2, cell_h = 66;
    const int y0 = BH - cell_h * rows, cw = BW / cols;
    for (int y = y0; y < BH; y++)
        for (int xx = 0; xx < BW; xx++) blend(xx, y, 0x05080F, 165);
    rect(0, y0, BW, 1, c565(0x67A9D8));
    for (int i = 0; i < st->nh && i < cols * rows; i++) {
        int cx = (i % cols) * cw + cw / 2, cy = y0 + (i / cols) * cell_h;
        if (st->hours[i].hour < 0) snprintf(buf, sizeof(buf), "AGORA");
        else snprintf(buf, sizeof(buf), "%dH", st->hours[i].hour);
        text5(buf, cx - tw5(buf, 1) / 2, cy + 6, 1, c565(0xBFD4E6), -1);
        int ic = st->hours[i].icon;
        if (ic >= 0 && ic < st->nhicons) icon_raw(st->hicon[ic], st->hpal[ic], cx - 8, cy + 16, 1);
        snprintf(buf, sizeof(buf), "%d" DEG, st->hours[i].temp);
        text5(buf, cx - tw5(buf, 2) / 2, cy + 36, 2, ink, -1);
        int r = st->hours[i].rain;
        snprintf(buf, sizeof(buf), "*%d%%", r);
        text5(buf, cx - tw5(buf, 1) / 2, cy + 54, 1, c565(r >= 30 ? 0x8CC4FF : 0x8B98A5), -1);
    }
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

// ---------- mostrador 4: temporizador e alarme ----------
// Duas metades (lado a lado na horizontal, uma por cima da outra na vertical), cada uma com uma caixa
// de 160 px: título, setas por cima e por baixo de cada par de dígitos, os dígitos, botões e uma linha de
// estado. As zonas de toque saem da mesma geometria (tz_box), para o desenho e o toque coincidirem.

typedef struct { int x, y, w, h; } crect_t;

static void tz_origin(int w, int h, int half, int *bx, int *by)
{
    if (w > h) {
        *bx = half * (w / 2) + (w / 2 - 160) / 2;
        *by = (h - 160) / 2;
    } else {
        *bx = (w - 160) / 2;
        *by = half * (h / 2) + (h / 2 - 160) / 2;
    }
}

// Caixa da zona z (coordenadas do ecrã). Metade 0 = temporizador, 1 = alarme.
static crect_t tz_box(int w, int h, int z)
{
    static const struct { int8_t half; uint8_t x, y, w, h; } Z[] = {
        [CZ_TMIN_UP] = {0, 22, 18, 56, 26}, [CZ_TMIN_DN] = {0, 22, 80, 56, 26},
        [CZ_TSEC_UP] = {0, 82, 18, 56, 26}, [CZ_TSEC_DN] = {0, 82, 80, 56, 26},
        [CZ_TGO] = {0, 8, 110, 70, 30},     [CZ_TRESET] = {0, 82, 110, 70, 30},
        [CZ_AH_UP] = {1, 22, 18, 56, 26},   [CZ_AH_DN] = {1, 22, 80, 56, 26},
        [CZ_AM_UP] = {1, 82, 18, 56, 26},   [CZ_AM_DN] = {1, 82, 80, 56, 26},
        [CZ_AON] = {1, 8, 110, 144, 30},
    };
    int bx, by;
    tz_origin(w, h, Z[z].half, &bx, &by);
    return (crect_t){bx + Z[z].x, by + Z[z].y, Z[z].w, Z[z].h};
}

static bool tz_running(const clock_state_t *st) { return st->tend > 0; }

int clockface_zone(const clock_state_t *st, int w, int h, int x, int y)
{
    if (st->face != CLK_FACE_TIMER) return CZ_NONE;
    for (int z = CZ_TMIN_UP; z <= CZ_AON; z++) {
        if (tz_running(st) && z >= CZ_TMIN_UP && z <= CZ_TSEC_DN) continue;   // a contar: sem setas
        if (st->tleft > 0 && z >= CZ_TMIN_UP && z <= CZ_TSEC_DN) continue;    // em pausa: idem
        crect_t r = tz_box(w, h, z);
        if (x >= r.x - 2 && x < r.x + r.w + 2 && y >= r.y - 2 && y < r.y + r.h + 2) return z;
    }
    return CZ_NONE;
}

static void tz_arrow(const clock_state_t *st, int z, bool up, uint32_t col, bool on)
{
    crect_t r = tz_box(BW, BH, z);
    if (on && st->hot == z) {
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++) blend(x, y, col, 70);
    }
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    uint16_t c = c565(on ? col : 0x2A3346);
    for (int k = 0; k < 7; k++) {   // triângulo de 13 x 7
        int yy = up ? cy - 3 + k : cy + 3 - k;
        rect(cx - k, yy, 2 * k + 1, 1, c);
    }
}

static void tz_button(const clock_state_t *st, int z, uint32_t col, bool filled)
{
    crect_t r = tz_box(BW, BH, z);
    bool hot = st->hot == z;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) blend(x, y, col, hot ? 150 : filled ? 90 : 30);
    uint16_t e = c565(col);
    rect(r.x, r.y, r.w, 2, e);
    rect(r.x, r.y + r.h - 2, r.w, 2, e);
    rect(r.x, r.y, 2, r.h, e);
    rect(r.x + r.w - 2, r.y, 2, r.h, e);
}

static void tz_pair(int v, int x, int y, uint16_t col, uint16_t ghost)
{
    char d[8];
    snprintf(d, sizeof(d), "%02d", (v < 0 ? -v : v) % 100);
    text5("88", x, y, 4, ghost, -1);
    text5(d, x, y, 4, col, -1);
}

static void face_timer(const clock_state_t *st, const struct tm *tm)
{
    const uint32_t CY = 0x5EF2FF, AM = 0xFFB000, BG = 0x0A0F1C;
    rect(0, 0, BW, BH, c565(BG));
    for (int y = 3; y < BH; y += 6)   // grelha de pontos discreta
        for (int x = 3; x < BW; x += 6) put(x, y, c565(0x141B2C));
    if (BW > BH) rect(BW / 2, 8, 1, BH - 16, c565(0x22304A));
    else rect(8, BH / 2, BW - 16, 1, c565(0x22304A));
    const uint16_t ghost = c565(0x18233A), ink = c565(0xFFFFFF), sub = c565(0x8B98A5);
    char buf[32];
    int bx, by;

    // ---- temporizador
    tz_origin(BW, BH, 0, &bx, &by);
    bool run = tz_running(st), paused = !run && st->tleft > 0;
    int left = run ? (int)(st->tend - st->now) : paused ? st->tleft : st->tset;
    if (left < 0) left = 0;
    text5("TEMPORIZADOR", bx + 6, by + 4, 1, c565(CY), -1);
    bool edit = !run && !paused;
    tz_arrow(st, CZ_TMIN_UP, true, CY, edit);
    tz_arrow(st, CZ_TMIN_DN, false, CY, edit);
    tz_arrow(st, CZ_TSEC_UP, true, CY, edit);
    tz_arrow(st, CZ_TSEC_DN, false, CY, edit);
    bool blink_off = paused && (st->now % 2);
    uint16_t dc = blink_off ? c565(0x2E5A66) : run ? c565(CY) : ink;
    tz_pair(left / 60, bx + 28, by + 47, dc, ghost);
    text5(":", bx + 76, by + 47, 4, (run && st->now % 2) ? ghost : dc, -1);
    tz_pair(left % 60, bx + 88, by + 47, dc, ghost);
    // botão 1: iniciar / pausar / retomar (ícones); botão 2: zerar (quadrado)
    tz_button(st, CZ_TGO, CY, run);
    crect_t g = tz_box(BW, BH, CZ_TGO);
    int gx = g.x + g.w / 2, gy = g.y + g.h / 2;
    if (run) {
        rect(gx - 7, gy - 8, 5, 16, c565(CY));
        rect(gx + 2, gy - 8, 5, 16, c565(CY));
    } else {
        for (int k = 0; k < 9; k++) rect(gx - 5 + k, gy - 8 + k, 1, 17 - 2 * k, c565(CY));
    }
    tz_button(st, CZ_TRESET, CY, false);
    crect_t rr = tz_box(BW, BH, CZ_TRESET);
    rect(rr.x + rr.w / 2 - 7, rr.y + rr.h / 2 - 7, 14, 14, c565(CY));
    if (run && st->has_time) {
        time_t endt = (time_t)st->tend;
        struct tm et;
        gmtime_r(&endt, &et);
        snprintf(buf, sizeof(buf), "TERMINA %02d:%02d", et.tm_hour, et.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "%s", paused ? "EM PAUSA" : st->tset ? "PRONTO" : "ESCOLHA O TEMPO");
    }
    text5(buf, bx + 80 - tw5(buf, 1) / 2, by + 148, 1, run ? c565(CY) : sub, -1);

    // ---- alarme
    tz_origin(BW, BH, 1, &bx, &by);
    text5("ALARME", bx + 6, by + 4, 1, c565(AM), -1);
    if (st->has_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
        text5(buf, bx + 154 - tw5(buf, 1), by + 4, 1, sub, -1);
    }
    tz_arrow(st, CZ_AH_UP, true, AM, true);
    tz_arrow(st, CZ_AH_DN, false, AM, true);
    tz_arrow(st, CZ_AM_UP, true, AM, true);
    tz_arrow(st, CZ_AM_DN, false, AM, true);
    uint16_t ac = st->alon ? c565(AM) : c565(0x9A8A6A);
    tz_pair(st->alh, bx + 28, by + 47, ac, ghost);
    text5(":", bx + 76, by + 47, 4, ac, -1);
    tz_pair(st->alm, bx + 88, by + 47, ac, ghost);
    tz_button(st, CZ_AON, AM, st->alon);
    crect_t ab = tz_box(BW, BH, CZ_AON);
    const char *al = st->alon ? "DESLIGAR" : "LIGAR";
    text5(al, ab.x + (ab.w - tw5(al, 2)) / 2, ab.y + 8, 2, st->alon ? ink : c565(AM), -1);
    if (st->alon && st->has_time) {
        int now_m = tm->tm_hour * 60 + tm->tm_min, at = st->alh * 60 + st->alm;
        int d = (at - now_m + 1440) % 1440;
        if (d == 0) d = 1440;
        snprintf(buf, sizeof(buf), "TOCA EM %dH%02d", d / 60, d % 60);
    } else {
        snprintf(buf, sizeof(buf), "%s", st->alon ? "LIGADO" : "DESLIGADO");
    }
    text5(buf, bx + 80 - tw5(buf, 1) / 2, by + 148, 1, st->alon ? c565(AM) : sub, -1);
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
        for (int i = 0; i < CLK_FACES; i++)
            rect(BW / 2 - CLK_FACES * 6 + 3 + i * 12, BH - 8, 6, 4, c565(i == st->face ? 0xFFFFFF : 0x6E6E78));
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
    return CLK_HAS_SCENE(st->face) && st->anim != CLK_ANIM_NONE;
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
    else if (st->face == 2) face_forecast(st, tm, t);
    else if (st->face == CLK_FACE_TIMER) face_timer(st, tm);
    else face_landscape(st, tm, t);   // 0 paisagem e 3 cidade (muda só o fundo)
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
