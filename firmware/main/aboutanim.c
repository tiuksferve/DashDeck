// Animação "Sobre o DashDeck" (toque no nome DashDeck no topo da tela inicial). Duas cenas em pixel art,
// alternadas a cada toque: 0 "Aurora" (serra à noite com aurora boreal e estrelas cadentes, no estilo da
// paisagem do relógio) e 1 "Hiperespaço" (salto entre estrelas até um planeta com anéis).
// Desenho direto no buffer RGB565. Contas em float só por objeto (estrela, coluna); por pixel, inteiras
// (o C6 não tem FPU). Os créditos (versão, autor, endereço) são etiquetas do LVGL por cima, no ui.c.
#include "aboutanim.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "clockface.h"

static uint16_t *B;
static int BW, BH, BS;
static int s_var;
static uint32_t s_seed;

#define LOGO "DASHDECK"
#define MAXC 700
static int16_t s_cells[MAXC][3];
static int s_ncells, s_lsc, s_ly;

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
    if (x >= 0 && y >= 0 && x < BW && y < BH) B[y * BS + x] = c;
}

static inline uint32_t get888(int x, int y)
{
    uint16_t p = B[y * BS + x];
    return (uint32_t)(((p >> 11) << 3) << 16 | (((p >> 5) & 0x3F) << 2) << 8 | ((p & 0x1F) << 3));
}

static void blend(int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= BW || y >= BH || a <= 0) return;
    if (a > 256) a = 256;
    B[y * BS + x] = c565(mix(get888(x, y), c, a));
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(i, j, c);
}

static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) put(cx + x, cy + y, c);
}

static float frand(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / 16777216.0f;
}

static float clampf(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

// céu em degradê com pontilhado entre faixas (como a paisagem do relógio), da linha y0 a y1
static void sky(uint32_t top, uint32_t bot, int y0, int y1)
{
    uint16_t band[14];
    for (int i = 0; i < 14; i++) band[i] = c565(mix(top, bot, i * 256 / 12));
    int h = y1 - y0 < 1 ? 1 : y1 - y0;
    for (int y = y0; y < y1 && y < BH; y++) {
        int f = (y - y0) * 12 * 256 / h, b = f >> 8, fr = f & 255;
        uint16_t *row = B + y * BS;
        for (int x = 0; x < BW; x++) row[x] = band[(fr > 128 && ((x + y) & 1) == 0) ? b + 1 : b];
    }
}

// nome em pixels grandes, revelado em pontilhado (0..16), com contorno escuro
static void logo(int reveal, uint32_t col, uint32_t outc)
{
    if (reveal <= 0) return;
    for (int i = 0; i < s_ncells; i++)
        for (int j = -1; j <= s_lsc; j++)
            for (int k = -1; k <= s_lsc; k++) blend(s_cells[i][0] + k, s_cells[i][1] + j, outc, reveal * 12);
    uint16_t c = c565(col);
    for (int i = 0; i < s_ncells; i++)
        for (int j = 0; j < s_lsc; j++)
            for (int k = 0; k < s_lsc; k++) {
                int x = s_cells[i][0] + k, y = s_cells[i][1] + j;
                if (BAYER[(y & 3) * 4 + (x & 3)] < reveal) put(x, y, c);
            }
}


static int16_t s_star[90][3];

// ======================================================================= 0: Aurora
static int16_t s_far[480], s_mid[480];

static void aurora_init(void)
{
    int HZ = BH * 74 / 100;
    for (int x = 0; x < BW && x < 480; x++) {
        float f = HZ - BH * 0.24f - (13 * sinf(x * .022f + 1.3f) + 8 * sinf(x * .061f + .4f) + 3 * sinf(x * .14f + 2));
        s_far[x] = (int16_t)(lroundf(f / 2) * 2);
        s_mid[x] = (int16_t)lroundf(HZ - 12 - (5 * sinf(x * .035f + 2.1f) + 4 * sinf(x * .09f) + 2 * sinf(x * .21f + 1)));
    }
}

static void aurora_draw(float t)
{
    const int HZ = BH * 74 / 100;
    sky(0x050818, 0x1A1440, 0, HZ);
    // estrelas a cintilar
    for (int i = 0; i < 90; i++) {
        if (s_star[i][1] >= HZ - 20) continue;
        int a = (int)(140 + 100 * sinf(t * 2.3f + s_star[i][2]));
        blend(s_star[i][0], s_star[i][1], 0xFFFFF0, a);
    }
    // aurora: cortinas verticais que ondulam; mais intensas em baixo, a desvanecer para cima
    for (int x = 0; x < BW; x++) {
        float fx = (float)x;
        float base = HZ * 0.52f + 10 * sinf(fx * 0.018f + t * 0.7f) + 6 * sinf(fx * 0.047f - t * 1.1f);
        float inten = 0.55f + 0.45f * sinf(fx * 0.031f + t * 0.9f) * sinf(fx * 0.013f - t * 0.4f + 1);
        if (inten < 0.1f) continue;
        int h = (int)(18 + 22 * inten), y0 = (int)base;
        int ai = (int)(inten * 170);
        uint32_t c0 = mix(0x3CFFB0, 0x50E0FF, (int)(128 + 127 * sinf(fx * 0.02f + t)));
        for (int k = 0; k < h; k++) {
            int y = y0 - k;
            if (y < 0) break;
            int a = ai * (h - k) / h;
            blend(x, y, k > h * 2 / 3 ? 0xB070FF : c0, a);
        }
        blend(x, y0 + 1, c0, ai / 3);
    }
    // estrela cadente a cada 3,5 s
    float ph = fmodf(t, 3.5f);
    if (ph < 0.8f) {
        int k = (int)(t / 3.5f);
        float sx = BW * (0.15f + 0.6f * ((k * 37) % 10) / 10.0f), sy = HZ * 0.12f + (k * 13) % 20;
        float p = ph / 0.8f;
        for (int i = 0; i < 18; i++) {
            float q = p - i * 0.02f;
            if (q < 0) break;
            blend((int)(sx + q * 90), (int)(sy + q * 34), 0xFFFFFF, (int)(240 * (1 - i / 18.0f) * (1 - p * 0.5f)));
        }
    }
    // montanhas (com um toque de verde da aurora no topo)
    uint16_t far = c565(0x161A3A), hill = c565(0x0A1214), rim = c565(0x2A5A58);
    for (int x = 0; x < BW; x++) {
        int fh = s_far[x < 480 ? x : 479], mh = s_mid[x < 480 ? x : 479];
        for (int y = fh; y < HZ; y++) B[y * BS + x] = y >= mh ? hill : far;
        put(x, fh, rim);
    }
    // cabana na margem, janela acesa e fumo
    int cx = BW * 22 / 100, cy = HZ - 2;
    rect(cx - 8, cy - 9, 16, 9, c565(0x2A1A12));
    for (int i = 0; i < 10; i++) rect(cx - 10 + i, cy - 9 - i / 2, 20 - 2 * i, 1, c565(0x3A2416));
    rect(cx + 2, cy - 6, 4, 3, c565(((int)(t * 3) % 7) ? 0xFFC860 : 0xFFE0A0));
    for (int i = 0; i < 6; i++) {
        float q = fmodf(t * 0.5f + i / 6.0f, 1);
        blend(cx + 5 + (int)(4 * sinf(q * 6 + t)), cy - 15 - (int)(q * 26), 0x9098B0, (int)(120 * (1 - q)));
    }
    // lago: reflexo ondulado e mais escuro
    int phs = (int)(t * 3 * 256);
    for (int y = HZ; y < BH; y++) {
        int sy2 = HZ - 1 - (y - HZ) * 2;
        if (sy2 < 0) sy2 = 0;
        int off = ((y * 230 + phs) / 256) % 3 - 1;
        for (int x = 0; x < BW; x++) {
            int xx = x + off < 0 ? 0 : x + off >= BW ? BW - 1 : x + off;
            uint16_t p = B[sy2 * BS + xx];
            int r = (p >> 11) * 5 / 8, g = ((p >> 5) & 0x3F) * 5 / 8, b = (p & 0x1F) * 2 / 3 + 2;
            B[y * BS + x] = (uint16_t)(r << 11 | g << 5 | (b > 31 ? 31 : b));
        }
    }
    logo((int)(clampf((t - 0.3f) / 1.4f) * 16), 0xFFFFFF, 0x0A0C1C);
}

// ======================================================================= 1: Hiperespaço
#define NWARP 110
static float s_wx[NWARP], s_wy[NWARP], s_wz[NWARP];

static void warp_init(void)
{
    for (int i = 0; i < NWARP; i++) {
        s_wx[i] = frand() * 2 - 1;
        s_wy[i] = frand() * 2 - 1;
        s_wz[i] = frand() * 0.9f + 0.1f;
    }
}

static void line(int x0, int y0, int x1, int y1, uint32_t c, int a0, int a1)
{
    int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (n == 0) {
        blend(x0, y0, c, a1);
        return;
    }
    for (int i = 0; i <= n; i++) blend(x0 + dx * i / n, y0 + dy * i / n, c, a0 + (a1 - a0) * i / n);
}

static void planet(int cx, int cy, int r, float t)
{
    // anel por trás
    int rr = r * 19 / 10;
    for (int k = 0; k < 360; k += 1) {
        float a = k * 0.0174533f;
        float s = sinf(a);
        if (s > 0) continue;   // metade de trás
        for (int w = 0; w < 4; w++) {
            int x = cx + (int)((rr - w * 3) * cosf(a)), y = cy + (int)((rr - w * 3) * 0.28f * s);
            blend(x, y, w == 1 ? 0xC8A878 : 0xE0C898, 150);
        }
    }
    // corpo com faixas que andam devagar e sombra à direita
    static const uint32_t BANDS[6] = {0xD99A5E, 0xE8C08A, 0xB86E48, 0xF0D6A8, 0xC98458, 0xE2B07A};
    int shift = (int)(t * 6);
    for (int y = -r; y <= r; y++) {
        int half = (int)sqrtf((float)(r * r - y * y));
        int bi = ((y + r) * 6 / (2 * r + 1) + ((y * 3 + shift) / 23)) % 6;
        if (bi < 0) bi += 6;
        uint32_t base = BANDS[bi];
        for (int x = -half; x <= half; x++) {
            int lit = 256 - ((x + r) * 180) / (2 * r);                         // luz da esquerda
            int edge = (x * x + y * y) * 90 / (r * r);
            int a = lit - edge;
            if (a < 30) a = 30;
            uint32_t c = mix(0x1A0E18, base, a);
            int px = cx + x, py = cy + y;
            if (BAYER[(py & 3) * 4 + (px & 3)] * 16 > a + 40) c = mix(c, 0x1A0E18, 60);
            put(px, py, c565(c));
        }
    }
    // anel à frente
    for (int k = 0; k < 360; k += 1) {
        float a = k * 0.0174533f;
        float s = sinf(a);
        if (s <= 0) continue;
        for (int w = 0; w < 4; w++) {
            int x = cx + (int)((rr - w * 3) * cosf(a)), y = cy + (int)((rr - w * 3) * 0.28f * s);
            blend(x, y, w == 1 ? 0xC8A878 : 0xF0DCB0, 210);
        }
    }
}

static void ship(int x, int y, float t)
{
    // pequena nave a apontar para a direita, com chama a tremer
    static const char *S[7] = {"..##.....", ".####....", "########.", "#######CC", "########.", ".####....",
                               "..##....."};
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 9; k++) {
            char ch = S[r][k];
            if (ch == '#') put(x + k, y + r, c565(r == 3 ? 0xB8C4D8 : 0xE6EDF3));
            else if (ch == 'C') put(x + k, y + r, c565(0x5EF2FF));
        }
    int fl = 3 + (int)(3 * fabsf(sinf(t * 23)));
    for (int i = 0; i < fl; i++) {
        put(x - 1 - i, y + 3, c565(i < 2 ? 0xFFF0A0 : 0xFF8A3C));
        if (i < fl - 2) {
            put(x - 1 - i, y + 2, c565(0xFF6A2C));
            put(x - 1 - i, y + 4, c565(0xFF6A2C));
        }
    }
}

// salto: riscos de estrelas a partir do centro, a acelerar até TW, e um clarão de 0,25 s à saída
static void warp_streaks(float t, float TW, float acc)
{
    int cx = BW / 2, cy = BH / 2;
    {
        float sp = t < TW ? 0.15f + t * t * acc : 3.4f;   // acelera
        for (int i = 0; i < NWARP; i++) {
            float z = s_wz[i] - fmodf(t * sp * 0.6f + i * 0.0091f, 1.0f);
            while (z <= 0.04f) z += 1.0f;
            float z2 = z + 0.02f + sp * 0.05f;
            int x1 = cx + (int)(s_wx[i] / z * BW * 0.25f), y1 = cy + (int)(s_wy[i] / z * BH * 0.3f);
            int x0 = cx + (int)(s_wx[i] / z2 * BW * 0.25f), y0 = cy + (int)(s_wy[i] / z2 * BH * 0.3f);
            int a = (int)(clampf(1.2f - z) * 255);
            line(x0, y0, x1, y1, i % 5 ? 0xCFE2FF : 0x9AB8FF, a / 4, a);
        }
        if (t > TW) {   // clarão na saída do salto
            int a = (int)((1 - (t - TW) / 0.25f) * 220);
            for (int y = 0; y < BH; y++)
                for (int x = 0; x < BW; x++) blend(x, y, 0xE8F0FF, a);
        }
    }
}

static int s_moon_x, s_moon_y;
static bool s_moon_vis;
static float s_x_t0 = -1;

static void warp_draw(float t)
{
    const float TW = 2.4f;   // fim do salto
    rect(0, 0, BW, BH, c565(0x03040C));
    s_moon_vis = false;
    if (t < TW + 0.25f) {
        warp_streaks(t, TW, 0.55f);
        if (t < TW) return;   // durante o salto, só as estrelas (o nome e os créditos vêm com o planeta)
    }
    // chegada: estrelas paradas, nebulosa ténue, planeta com anéis, lua e nave
    for (int i = 0; i < 90; i++) {
        int a = (int)(150 + 90 * sinf(t * 2 + s_star[i][2]));
        blend(s_star[i][0], s_star[i][1], i % 7 ? 0xFFFFFF : 0xFFD8A0, a);
    }
    for (int y = BH / 3; y < BH; y += 2)
        for (int x = 0; x < BW * 45 / 100; x += 2) {
            int d = (x - BW / 6) * (x - BW / 6) / 40 + (y - BH * 2 / 3) * (y - BH * 2 / 3) / 12;
            if (d < 200) blend(x + (y & 2) / 2, y, 0x6A3CA0, (200 - d) / 5);
        }
    int pr = BH * 18 / 100, px = BW * 76 / 100, py = BH * 52 / 100;
    planet(px, py, pr, t);
    float ma = t * 0.8f;
    int mx = px + (int)(pr * 2.3f * cosf(ma)), my = py + (int)(pr * 0.6f * sinf(ma));
    if (sinf(ma) < 0 || mx < px - pr || mx > px + pr) {
        disc(mx, my, 4, c565(0xB8C0D0));
        put(mx + 1, my - 1, c565(0x8890A0));   // crateras
        put(mx - 2, my + 1, c565(0x8890A0));
        put(mx - 1, my - 2, c565(0xE6EAF2));
        s_moon_x = mx;
        s_moon_y = my;
        s_moon_vis = true;
    }
    float sp = fmodf(t - TW, 6.0f);
    ship((int)(-20 + sp * (BW + 40) / 6.0f), BH * 30 / 100 + (int)(4 * sinf(t * 1.5f)), t);
    logo((int)(clampf((t - TW - 0.25f) / 0.5f) * 16), 0xFFFFFF, 0x03040C);
}


// =======================================================================
#define NCRAT 14
static int16_t s_crat[NCRAT][3];

static void x_init(void)
{
    uint32_t keep = s_seed;
    s_seed = 99;
    for (int i = 0; i < NCRAT; i++) {
        s_crat[i][0] = (int16_t)(frand() * BW);
        s_crat[i][1] = (int16_t)(BH * 0.72f + frand() * BH * 0.28f);
        s_crat[i][2] = (int16_t)(3 + frand() * 9);
    }
    s_seed = keep;
}

static int ground_y(int x)   // horizonte curvo da lua
{
    int d = x - BW / 2;
    return BH * 66 / 100 + d * d / (BW * 3);
}

// píxel "grande" (2 x 2) a partir de uma origem: a nave e o astronauta ficam ao dobro do tamanho
static int s_ox, s_oy;
static void p2(int x, int y, uint16_t c)
{
    rect(s_ox + x * 2, s_oy + y * 2, 2, 2, c);
}

static void lander(int x, int y, bool legs, bool flame, float t)
{
    s_ox = x;
    s_oy = y;
    // nave de pé (nariz para cima), 9 x 13 px; x, y = centro da base
    static const char *S[11] = {"....#....", "...###...", "...#C#...", "..#####..", "..#####..", ".#######.",
                                ".#######.", ".#######.", "#########", "##.###.##", "#...#...#"};
    for (int r = 0; r < 11; r++)
        for (int k = 0; k < 9; k++) {
            char ch = S[r][k];
            if (r == 10 && !legs) continue;
            if (ch == '#') p2(-4 + k, -11 + r, c565(k < 4 ? 0xE6EDF3 : 0xB8C4D8));
            else if (ch == 'C') p2(-4 + k, -11 + r, c565(0x5EF2FF));
        }
    if (legs) {
        p2(-5, -1, c565(0xB8C4D8));
        p2(5, -1, c565(0xB8C4D8));
    }
    if (flame) {
        int fl = 3 + (int)(4 * fabsf(sinf(t * 25)));
        for (int i = 0; i < fl; i++) {
            p2(0, i, c565(i < 2 ? 0xFFF0A0 : 0xFF8A3C));
            if (i < fl - 1) {
                p2(-1, i, c565(0xFF6A2C));
                p2(1, i, c565(0xFF6A2C));
            }
        }
    }
}

static void x_draw(float e, float t)
{
    const float TW = 1.6f;
    rect(0, 0, BW, BH, c565(0x03040C));
    if (e < TW + 0.25f) {
        warp_streaks(e, TW, 1.6f);
        if (e < TW) return;
    }
    float s = e - TW;
    for (int i = 0; i < 90; i++) {
        if (s_star[i][1] > BH * 0.7f) continue;
        blend(s_star[i][0], s_star[i][1], 0xFFFFFF, (int)(150 + 90 * sinf(t * 2 + s_star[i][2])));
    }
    // o planeta dos anéis, grande, a nascer no horizonte da lua
    int pr = BH * 24 / 100;
    planet(BW * 72 / 100, BH * 34 / 100, pr, t);
    // superfície da lua: cinza com pontilhado, crateras e sombra para a direita
    for (int x = 0; x < BW; x++) {
        int gy = ground_y(x);
        for (int y = gy; y < BH; y++) {
            int depth = (y - gy) * 256 / (BH - gy + 1);
            uint32_t c = mix(0xB8BCC6, 0x6A6E7A, depth * 3 / 4 + x * 64 / BW);
            if (BAYER[(y & 3) * 4 + (x & 3)] < 3) c = mix(c, 0x50545E, 90);
            put(x, y, c565(c));
        }
        put(x, gy, c565(0xD8DCE4));
    }
    for (int i = 0; i < NCRAT; i++) {
        int cx = s_crat[i][0], cy = s_crat[i][1], cr = s_crat[i][2];
        if (cy < ground_y(cx) + 3) continue;
        for (int y = -cr / 2; y <= cr / 2; y++)
            for (int x = -cr; x <= cr; x++) {
                int d = x * x + 4 * y * y;
                if (d > cr * cr) continue;
                int rim = d > (cr - 2) * (cr - 2);
                blend(cx + x, cy + y, rim ? (y < 0 ? 0x50545E : 0xE0E4EC) : 0x7A7E8A, rim ? 160 : 120);
            }
    }
    // a nave entra pela esquerda, abranda e desce na vertical até pousar
    int lx = BW * 38 / 100, ly = ground_y(lx);
    const float TD = 4.2f;
    if (s < TD) {
        float q = s / TD, ez = 1 - (1 - q) * (1 - q);
        int x = (int)(-20 + (lx + 20) * clampf(ez * 1.25f));
        int y = (int)(ly - 90 + 90 * ez);
        lander(x, y, q > 0.6f, true, t);
    } else {
        lander(lx, ly, true, false, t);
        float d = s - TD;
        if (d < 1.4f) {   // poeira a espalhar-se para os lados
            for (int i = 0; i < 16; i++) {
                float dir = (i < 8 ? -1.0f : 1.0f) * (0.3f + (i % 8) * 0.12f);
                int px = lx + (int)(dir * d * 60), py = ly - (int)((i % 3) * d * 9 * (1.4f - d));
                for (int k = 0; k < 4; k++) blend(px + (k & 1), py - (k >> 1), 0xD0D4DC, (int)(200 * (1 - d / 1.4f)));
            }
        }
        s_ox = lx;
        s_oy = ly;
        if (((int)(t * 2)) % 2) p2(0, -12, c565(0xFF3030));   // luz no topo a piscar
        if (d > 1.6f) {   // o astronauta sai e espeta a bandeira
            s_ox = lx + 14 + (int)(clampf((d - 1.6f) / 1.5f) * 26);
            s_oy = ly;
            int step = (d < 3.1f) && ((int)(t * 6) % 2);
            uint16_t w = c565(0xF4F4F8);
            p2(-1, -7, w); p2(0, -7, w); p2(-1, -6, w); p2(0, -6, c565(0xE3B341));   // capacete e visor
            for (int y = -5; y <= -3; y++) { p2(-1, y, w); p2(0, y, w); }
            p2(-1 - step, -2, w); p2(0 + step, -2, w);                                 // pernas
            p2(1, -5, c565(0xB8C4D8));                                                  // mochila
            if (d > 3.3f) {
                int fx = 4;
                for (int y = 1; y <= 10; y++) p2(fx, -y, c565(0xC0C4CC));
                int wv = (int)(t * 4) % 2;
                for (int y = 0; y < 3; y++)
                    for (int x = 1; x <= 5; x++) p2(fx + x, -10 + y + (x > 3 ? wv : 0), c565(0x58A6FF));
                p2(fx + 2, -9, c565(0xFFFFFF));
            }
        }
        if (d > 4.0f) {   // mensagem de missão cumprida
            const char *m = "TAHDAAAH!!!";
            int16_t (*c)[3] = s_cells + s_ncells;
            int n = clockfont_cells(m, (BW - clockfont_width(m, 2)) / 2, 10, 2, c, MAXC - s_ncells);
            int a = (int)(clampf((d - 4.0f) / 0.5f) * 256);
            for (int i = 0; i < n; i++)
                for (int j = -1; j <= 2; j++)
                    for (int k = -1; k <= 2; k++) blend(c[i][0] + k, c[i][1] + j, 0x03040C, a);
            for (int i = 0; i < n; i++)
                for (int j = 0; j < 2; j++)
                    for (int k = 0; k < 2; k++) blend(c[i][0] + k, c[i][1] + j, 0xFFD64D, a);
        }
    }
}

// =======================================================================
void aboutanim_start(int variant, int w, int h)
{
    BW = w;
    BH = h;
    s_var = ((variant % ABOUTANIM_VARIANTS) + ABOUTANIM_VARIANTS) % ABOUTANIM_VARIANTS;
    s_seed = 7;
    for (int i = 0; i < 90; i++) {
        s_star[i][0] = (int16_t)(frand() * w);
        s_star[i][1] = (int16_t)(frand() * h);
        s_star[i][2] = (int16_t)(frand() * 6);
    }
    s_lsc = w >= 240 ? 4 : 3;
    s_ly = h >= 240 ? 26 : 12;
    s_ncells = clockfont_cells(LOGO, (w - clockfont_width(LOGO, s_lsc)) / 2, s_ly, s_lsc, s_cells, MAXC);
    s_x_t0 = -1;
    if (s_var == 0) aurora_init();
    else {
        warp_init();
        x_init();
    }
}

void aboutanim_draw(uint16_t *buf, int stride_px, float t)
{
    B = buf;
    BS = stride_px;
    if (s_var == 0) aurora_draw(t);
    else if (s_x_t0 >= 0) x_draw(t - s_x_t0, t);
    else warp_draw(t);
}

float aboutanim_credits_at(int variant)
{
    return (variant % ABOUTANIM_VARIANTS) == 1 ? 2.65f : 0.0f;   // Hiperespaço: depois do salto
}

bool aboutanim_touch(int x, int y, float t)
{
    if (s_var != 1 || s_x_t0 >= 0 || !s_moon_vis) return false;
    int dx = x - s_moon_x, dy = y - s_moon_y;
    if (dx * dx + dy * dy > 18 * 18) return false;   // a lua é pequena: margem generosa para o dedo
    s_x_t0 = t;
    return true;
}

const char *aboutanim_name(int variant)
{
    static const char *N[ABOUTANIM_VARIANTS] = {"Aurora", "Hiperespaço"};
    return N[((variant % ABOUTANIM_VARIANTS) + ABOUTANIM_VARIANTS) % ABOUTANIM_VARIANTS];
}
