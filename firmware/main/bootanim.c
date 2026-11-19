// Animação de arranque (3,5 s), escolhida ao acaso: "Pixels" (o nome forma-se com pixels que voam,
// ondas de rádio) ou "Nascer do sol" (a paisagem do relógio passa da noite ao dia).
// Desenho direto no buffer RGB565, com contas inteiras por pixel (o C6 não tem FPU).
#include "bootanim.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "clockface.h"

static uint16_t *B;
static int BW, BH, BS;
static int s_variant;

#define LOGO "DASHDECK"
// As sequências foram desenhadas para 3,5 s; esticam-se para durar BOOTANIM_SECONDS.
#define SEQ(tr) (((tr) > BOOTANIM_SECONDS ? BOOTANIM_SECONDS : (tr)) * (3.5f / BOOTANIM_SECONDS))
#define MAXC 1024

static inline uint16_t c565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

// mistura dois RGB888 com a (0..256)
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
    if (x >= 0 && y >= 0 && x < BW && y < BH) B[y * BS + x] = c;
}

static void blend(int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= BW || y >= BH || a <= 0) return;
    if (a > 256) a = 256;
    uint16_t p = B[y * BS + x];
    uint32_t old = (uint32_t)(((p >> 11) << 3) << 16 | (((p >> 5) & 0x3F) << 2) << 8 | ((p & 0x1F) << 3));
    B[y * BS + x] = c565(mix(old, c, a));
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(i, j, c);
}

static void ring(int cx, int cy, int r, uint32_t c, int a)
{
    int n = r * 7 < 24 ? 24 : r * 7;
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

static uint32_t s_seed;
static float frand(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / 16777216.0f;
}

// ---------- comum ----------
static int16_t s_cells[MAXC][3];
static int s_ncells;
static int16_t s_star[70][3];

// A frase por baixo do nome: numa linha na horizontal; na vertical parte-se depois da vírgula.
// out >= 0 desenha um contorno (RGB888) para se ler sobre o céu.
static void tagline(int y, uint32_t col, int32_t out)
{
    static const char *one[] = {"TELA PEQUENA, GRANDES POSSIBILIDADES!"};
    static const char *two[] = {"TELA PEQUENA,", "GRANDES POSSIBILIDADES!"};
    bool fits = clockfont_width(one[0], 1) <= BW - 8;
    const char **lines = fits ? one : two;
    int16_t (*c)[3] = s_cells + s_ncells;
    for (int l = 0; l < (fits ? 1 : 2); l++) {
        int n = clockfont_cells(lines[l], (BW - clockfont_width(lines[l], 1)) / 2, y + l * 10, 1, c, MAXC - s_ncells);
        if (out >= 0)
            for (int i = 0; i < n; i++) rect(c[i][0] - 1, c[i][1] - 1, 3, 3, c565((uint32_t)out));
        for (int i = 0; i < n; i++) put(c[i][0], c[i][1], c565(col));
    }
}

// ---------- Pixels ----------
static int16_t s_from[MAXC][2];
static uint8_t s_delay[MAXC];   // centésimos de s
static const uint32_t PAL[8] = {0x5EF2FF, 0x58A6FF, 0x8B7CFF, 0xD2A8FF, 0xFF7AB6, 0xFF9A5A, 0xFFB000, 0xFFD64D};

static void pixels_init(void)
{
    int sc = 3, lx = (BW - clockfont_width(LOGO, sc)) / 2, ly = BH / 2 - 20;
    s_ncells = clockfont_cells(LOGO, lx, ly, sc, s_cells, MAXC);
    s_seed = 11;
    for (int i = 0; i < s_ncells; i++) {
        s_from[i][0] = (int16_t)(frand() * BW);
        s_from[i][1] = (int16_t)(frand() * BH);
        s_delay[i] = (uint8_t)(frand() * 50);
    }
}

static void pixels_draw(float tr)
{
    float t = SEQ(tr);
    const uint32_t bg = 0x070B1C;
    rect(0, 0, BW, BH, c565(bg));
    for (int i = 0; i < 70; i++) {
        int a = (int)(64 + 64 * sinf(tr * 3 + s_star[i][2]));
        blend(s_star[i][0], s_star[i][1], 0xDCE6FF, a);
    }
    int cx = BW / 2, cy = BH / 2 - 10;
    if (tr > 2.0f)
        for (int k = 0; k < 3; k++) {
            float r = fmodf((tr - 2.0f) * 70 + k * 40, 120);
            if (r > 4) ring(cx, cy, (int)r, PAL[(k * 3) % 8], (int)(clampf(1 - r / 120) * 150));
        }
    for (int i = 0; i < s_ncells; i++) {
        float e = ease((t - 0.15f - s_delay[i] / 100.0f) / 1.5f);
        int x = (int)lroundf(s_from[i][0] + (s_cells[i][0] - s_from[i][0]) * e);
        int y = (int)lroundf(s_from[i][1] + (s_cells[i][1] - s_from[i][1]) * e);
        uint32_t c = PAL[s_cells[i][2] % 8];
        rect(x, y, 3, 3, c565(e < 1 ? mix(bg, c, (int)(102 + 154 * e)) : c));
    }
    if (t > 2.2f) tagline(cy + 28, mix(bg, 0x9AA4C0, (int)(clampf((t - 2.2f) / 0.5f) * 256)), -1);
}

// ---------- Nascer do sol ----------
static int16_t s_far[320], s_mid[320];
static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static int s_lsc, s_lx;

static void sunrise_init(void)
{
    int HZ = BH - 22;
    for (int x = 0; x < BW && x < 320; x++) {
        float f = HZ - 38 - (14 * sinf(x * .022f + 1.3f) + 8 * sinf(x * .061f + .4f) + 3 * sinf(x * .14f + 2));
        s_far[x] = (int16_t)(lroundf(f / 2) * 2);
        s_mid[x] = (int16_t)lroundf(HZ - 16 - (5 * sinf(x * .035f + 2.1f) + 4 * sinf(x * .09f) + 2 * sinf(x * .21f + 1)));
    }
    s_lsc = BW >= 200 ? 4 : 3;
    s_lx = (BW - clockfont_width(LOGO, s_lsc)) / 2;
    s_ncells = clockfont_cells(LOGO, s_lx, 30, s_lsc, s_cells, MAXC);
}

static void sunrise_draw(float tr)
{
    const int HZ = BH - 22;
    float t = SEQ(tr);   // sequência; tr = ambiente
    float L = ease(t / 3.2f);
    int l16 = (int)(clampf(L * 1.6f) * 256), l14 = (int)(clampf(L * 1.4f - .4f) * 256);
    int l18 = (int)(clampf(L * 1.8f) * 256), l15 = (int)(clampf(L * 1.5f - .6f) * 256);
    uint32_t top = mix(mix(0x070B24, 0x3A2C6A, l16), 0x2F6FD0, l14);
    uint32_t bot = mix(mix(0x1C2350, 0xF2884A, l18), 0xA3D6F7, l15);
    uint16_t band[13];
    for (int i = 0; i < 13; i++) band[i] = c565(mix(top, bot, i * 256 / 11));
    for (int y = 0; y < HZ; y++) {
        int f = y * 12 * 256 / HZ, b = f >> 8, fr = f & 255;
        for (int x = 0; x < BW; x++) B[y * BS + x] = band[(fr > 128 && ((x + y) & 1) == 0) ? b + 1 : b];
    }
    for (int i = 0; i < 70; i++) {
        if (s_star[i][1] >= HZ - 60) continue;
        int a = (int)((1 - L * 1.5f) * (128 + 100 * sinf(t * 2 + s_star[i][2])));
        blend(s_star[i][0], s_star[i][1], 0xFFFFF0, a);
    }
    int sx = BW * 74 / 100, sy = (int)lroundf(HZ - 58 * L);
    uint32_t suc = mix(0xFF7A3C, 0xFFF1A0, (int)(L * 256));
    uint16_t su = c565(suc);
    for (int y = -11; y <= 11; y++)
        for (int x = -11; x <= 11; x++) {
            int d = x * x + y * y;
            if (d <= 49) put(sx + x, sy + y, su);
            else if (d <= 121) blend(sx + x, sy + y, suc, 46);
        }
    int li = (int)(L * 256);
    uint16_t far = c565(mix(0x1C2244, 0x6A7AA8, li)), hill = c565(mix(0x0F1D1B, 0x3A6E40, li));
    uint16_t mead = c565(mix(0x15301F, 0x5A9A48, li));
    for (int x = 0; x < BW; x++) {
        int fh = x < 320 ? s_far[x] : s_far[319], mh = x < 320 ? s_mid[x] : s_mid[319];
        for (int y = fh; y < HZ; y++) B[y * BS + x] = y >= mh ? hill : far;
        for (int y = HZ - 5; y < HZ; y++) B[y * BS + x] = mead;
    }
    // lago: reflexo do que está por cima, mais escuro e ondulado
    int ph = (int)(tr * 3 * 256);
    for (int y = HZ; y < BH; y++) {
        int sy2 = HZ - 1 - (y - HZ) * 2;
        if (sy2 < 0) sy2 = 0;
        int off = ((y * 230 + ph) / 256) % 3 - 1;
        for (int x = 0; x < BW; x++) {
            int xx = x + off < 0 ? 0 : x + off >= BW ? BW - 1 : x + off;
            uint16_t p = B[sy2 * BS + xx];
            int r = (p >> 11) * 5 / 8, g = ((p >> 5) & 0x3F) * 5 / 8, b = (p & 0x1F) * 2 / 3 + 2;
            B[y * BS + x] = (uint16_t)(r << 11 | g << 5 | (b > 31 ? 31 : b));
        }
    }
    // nome: aparece pixel a pixel (pontilhado ordenado)
    int reveal = (int)(clampf((t - 0.6f) / 1.6f) * 16);
    uint16_t white = 0xFFFF;
    if (reveal > 0)   // contorno escuro, que escurece junto com o nome
        for (int i = 0; i < s_ncells; i++)
            for (int j = -1; j <= s_lsc; j++)
                for (int k = -1; k <= s_lsc; k++)
                    blend(s_cells[i][0] + k, s_cells[i][1] + j, 0x0E1020, reveal * 10);
    for (int i = 0; i < s_ncells; i++)
        for (int j = 0; j < s_lsc; j++)
            for (int k = 0; k < s_lsc; k++) {
                int x = s_cells[i][0] + k, y = s_cells[i][1] + j;
                if (BAYER[(y & 3) * 4 + (x & 3)] < reveal) put(x, y, white);
            }
    if (t > 2.3f) {   // a frase, depois do nome completo
        int a = (int)(clampf((t - 2.3f) / 0.5f) * 256);
        tagline(30 + 7 * s_lsc + 8, mix(top, 0xFFFFFF, a), (int32_t)mix(top, 0x0E1020, a));
    }
}

void bootanim_start(int variant, int w, int h)
{
    BW = w;
    BH = h;
    s_variant = variant & 1;
    s_seed = 5;
    for (int i = 0; i < 70; i++) {
        s_star[i][0] = (int16_t)(frand() * w);
        s_star[i][1] = (int16_t)(frand() * h);
        s_star[i][2] = (int16_t)(frand() * 6);
    }
    if (s_variant == 0) pixels_init();
    else sunrise_init();
}

void bootanim_draw(uint16_t *buf, int stride_px, float t, float progress, bool waiting)
{
    B = buf;
    BS = stride_px;
    if (s_variant == 0) pixels_draw(t);
    else sunrise_draw(t);
    // barra de progresso em baixo (contorno escuro para se ler sobre o céu e o lago)
    int bw = BW * 3 / 5, bx = (BW - bw) / 2, by = BH - 12;
    uint32_t fg = s_variant == 0 ? 0x9AA4C0 : 0xFFFFFF;
    rect(bx - 1, by - 1, bw + 2, 6, c565(0x0E1020));
    rect(bx, by, bw, 4, c565(s_variant == 0 ? 0x1A2140 : 0x3A4A70));
    rect(bx, by, (int)(bw * clampf(progress)), 4, c565(fg));
    if (waiting) {
        const char *s = "A AGUARDAR O UCONSOLE";
        int16_t (*c)[3] = s_cells + s_ncells;
        int n = clockfont_cells(s, (BW - clockfont_width(s, 1)) / 2, by - 12, 1, c, MAXC - s_ncells);
        uint16_t out = c565(0x0E1020), col = c565(fg);
        for (int i = 0; i < n; i++) rect(c[i][0] - 1, c[i][1] - 1, 3, 3, out);
        for (int i = 0; i < n; i++) put(c[i][0], c[i][1], col);
    }
}
