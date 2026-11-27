// Animações do Deck³ no estilo do DashDeck (ver anim.h). As sequências seguem as do DashDeck
// (bootanim.c "Pixels", notify.c "Ondas LoRa", aboutanim.c "Hiperespaço"), com os pixels a passarem a
// voxels: o cubo e os modelos dos avisos montam-se voxel a voxel (vox_view_t.fly).
#include "anim.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------------------------- comum

static uint32_t *B;
static int BW, BH, BS;

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
static inline int sup_sc(int sc)
{
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

#define LOGO "DECK\xC2\xB3"
#define TAGLINE "CUBO PEQUENO, GRANDES POSSIBILIDADES!"
#define BG 0x03060F
#define LOGO_SC 9
#define MAXC 400

static int16_t s_cells[MAXC][3];
static int16_t s_from[MAXC][2];
static uint8_t s_delay[MAXC];
static int s_ncells, s_logo_y, s_cube_y;

void anim_boot_start(int w, int h)
{
    BW = w;
    BH = h;
    stars_init(w, h);
    s_cube_y = h * 34 / 100;
    s_logo_y = h * 60 / 100;
    char f[16];
    fold(LOGO, f, sizeof(f));
    s_ncells = cells(f, (w - pfont_width(LOGO, LOGO_SC)) / 2, s_logo_y, LOGO_SC, s_cells, MAXC);
    s_seed = 11;
    for (int i = 0; i < s_ncells; i++) {
        s_from[i][0] = (int16_t)(frand() * w);
        s_from[i][1] = (int16_t)(frand() * h);
        s_delay[i] = (uint8_t)(frand() * 50);
    }
}

void anim_boot_draw(uint32_t *buf, int stride, float t, float progress, const char *status, float tilt)
{
    B = buf;
    BS = stride;
    fill(BG);
    stars_draw(t, 150);
    int cx = BW / 2, cy = s_cube_y;
    const vox_model_t *cube = vox_get("deck3");
    float ft = vox_fly_time(cube);
    // ondas de rádio a sair do cubo, depois de montado
    if (t > ft)
        for (int k = 0; k < 3; k++) {
            float r = fmodf((t - ft) * 80 + k * 55, 165);
            ring(cx, cy, 70 + (int)r, PAL[(k * 3) % 8], (int)(clampf(1 - r / 165) * 140));
        }
    // o cubo: voxels voam e montam-se camada a camada; depois roda devagar (o IMU inclina-o)
    vox_view_t v = VOX_VIEW_DEFAULT;
    v.scale = 34;
    v.cx = cx;
    v.cy = cy;
    v.yaw = 0.785f + t * 0.45f;
    v.pitch = 0.6f + tilt;
    v.fly = t < ft + 0.1f ? t + 1 : 0;
    vox_render(cube, &v, buf, BW, BH, stride);
    // o nome forma-se com pixels que voam (como no DashDeck)
    for (int i = 0; i < s_ncells; i++) {
        float e = ease((t - 0.9f - s_delay[i] / 100.0f) / 1.3f);
        if (e <= 0) continue;
        int x = (int)lroundf(s_from[i][0] + (s_cells[i][0] - s_from[i][0]) * e);
        int y = (int)lroundf(s_from[i][1] + (s_cells[i][1] - s_from[i][1]) * e);
        int ci = s_cells[i][2] & 0xFF, full = cell_sz(LOGO_SC, s_cells[i][2]) - 1;
        uint32_t c = s_cells[i][2] & SUP ? PAL[6] : PAL[(ci * 2) % 8];
        int sz = e < 1 ? 3 + (int)(e * (full - 3)) : full;   // cresce até ao bloco final
        rect(x, y, sz, sz, e < 1 ? mix(BG, c, (int)(102 + 154 * e)) : c);
    }
    int ty = s_logo_y + 7 * LOGO_SC + 22;
    if (t > 2.4f) {
        int a = (int)(clampf((t - 2.4f) / 0.5f) * 256);
        pfont_draw(buf, BW, BH, stride, TAGLINE, (BW - pfont_width(TAGLINE, 2)) / 2, ty, 2, mix(BG, 0x9AA4C0, a), 0, -1);
        B = buf; BS = stride;
    }
    // barra de progresso e estado da ligação
    int bw = BW * 3 / 5, bx = (BW - bw) / 2, by = BH - 26;
    rect(bx - 1, by - 1, bw + 2, 8, 0x0E1020);
    rect(bx, by, bw, 6, 0x1A2140);
    int fw = (int)(bw * clampf(progress));
    for (int x = 0; x < fw; x++) rect(bx + x, by, 1, 6, mix(PAL[0], PAL[3], x * 256 / bw));
    if (status && *status) {
        int sc = pfont_width(status, 2) <= BW - 16 ? 2 : 1;
        pfont_draw(buf, BW, BH, stride, status, (BW - pfont_width(status, sc)) / 2, by - 22, sc, 0x9AA4C0, 0, -1);
    }
}

// ------------------------------------------------------------------------------------- avisos

static const uint32_t NCOL[3][4] = {
    {0x2EA043, 0x67EA94, 0x0E3B1C, 0xE7FFE9},   // mensagem: verde LoRa
    {0xF2B21B, 0xFFE27A, 0x4A2A00, 0xFFF6E0},   // alarme: dourado
    {0xDA3633, 0xFF7B72, 0x3D0808, 0xFFECEC},   // emergência: vermelho
};
static int s_nkind;
static const vox_model_t *s_nmodel;
static char s_nname[40];
static int s_nlen;

uint32_t anim_notify_color(int kind, int which)
{
    return NCOL[kind < 0 || kind > 2 ? 0 : kind][which & 3];
}

static bool s_ntint;

void anim_notify_start(const vox_model_t *m, const char *name, int kind, bool tint)
{
    s_ntint = tint;
    s_nkind = kind < 0 || kind > 2 ? 0 : kind;
    s_nmodel = m;
    char f[96];
    s_nlen = fold(name ? name : "", f, sizeof(f));
    if (s_nlen > (int)sizeof(s_nname) - 1) s_nlen = sizeof(s_nname) - 1;
    memcpy(s_nname, f, s_nlen);
    s_nname[s_nlen] = '\0';
}

void anim_notify_draw(uint32_t *buf, int w, int h, int stride, float t)
{
    B = buf; BW = w; BH = h; BS = stride;
    for (int y = 0; y < h; y++) memset(&buf[y * stride], 0, w * 4);   // transparente
    const uint32_t *C = NCOL[s_nkind];
    int cx = w / 2, cy = h * 42 / 100;
    // ondas a fechar sobre o centro
    for (int k = 0; k < 3; k++) {
        float p = clampf((t - k * 0.15f) / 0.8f);
        if (p > 0 && p < 1) ring(cx, cy, (int)(160 - 135 * ease(p)), C[1], (int)(205 * (1 - p * 0.5f)));
    }
    if (!s_nmodel) return;
    // os voxels voam e montam o modelo (acaba por volta de 1,3 s)
    float ft = vox_fly_time(s_nmodel);
    float speed = ft / 0.85f;
    float fl = (t - 0.4f) * speed;
    vox_view_t v = VOX_VIEW_DEFAULT;
    int dim = s_nmodel->nx > s_nmodel->ny ? s_nmodel->nx : s_nmodel->ny;
    if (s_nmodel->nz > dim) dim = s_nmodel->nz;
    v.scale = (h * 0.5f) / (dim * 1.45f);
    v.cx = cx;
    v.cy = cy;
    // modelos de origem tingidos como no DashDeck: envelope verde, despertador dourado, avião vermelho
    if (s_ntint) v.tint = s_nkind == 1 ? C[1] : C[0];
    if (fl < 0) return;
    if (fl < ft + 0.05f) {
        v.fly = fl + 1;
    } else {
        float k = t - (0.4f + ft / speed);
        if (s_nkind == 2) {                 // emergência: pisca 3 vezes por segundo
            v.glow = ((int)(k * 6)) % 2 == 0 ? 1 : 0;
        } else if (s_nkind == 1) {          // alarme: abana em rajadas de 0,6 s
            float ph = fmodf(k, 1.0f);
            v.cx += ph < 0.6f ? (((int)(t * 28)) % 2 ? 4 : -4) : 0;
            v.glow = 0.5f;
        } else {                            // mensagem: pulsa e balança
            v.glow = 0.5f + 0.5f * sinf(k * 5);
            v.yaw += sinf(k * 1.8f) * 0.3f;
        }
        for (int j = 0; j < 2; j++) {       // ondas a sair
            float r = fmodf(k * 80 + j * 35, 80);
            ring(cx, cy, 80 + (int)r, C[1], (int)(clampf(1 - r / 80) * 170));
        }
    }
    vox_render(s_nmodel, &v, buf, w, h, stride);
    // o nome escreve-se letra a letra por baixo
    if (t > 1.3f && s_nlen) {
        int shown = (int)((t - 1.3f) / 0.07f);
        int sc = pfont_width(s_nname, 3) <= w - 8 ? 3 : 2;
        pfont_draw(buf, w, h, stride, s_nname, (w - pfont_width(s_nname, sc)) / 2, h - 7 * sc - 6, sc, C[3],
                   0x0E1A12, shown);
    }
}

// ------------------------------------------------------------------------------------- "Sobre"

#define NWARP 160
static float s_warp[NWARP][3];   // ângulo, distância inicial, velocidade

void anim_about_start(int w, int h)
{
    BW = w;
    BH = h;
    stars_init(w, h);
    s_seed = 23;
    for (int i = 0; i < NWARP; i++) {
        s_warp[i][0] = frand() * 6.2831853f;
        s_warp[i][1] = 4 + frand() * 60;
        s_warp[i][2] = 0.6f + frand() * 0.8f;
    }
    char f[16];
    fold(LOGO, f, sizeof(f));
    s_logo_y = h * 63 / 100;
    s_ncells = cells(f, (w - pfont_width(LOGO, 8)) / 2, s_logo_y, 8, s_cells, MAXC);
    s_seed = 31;
    for (int i = 0; i < s_ncells; i++) {
        s_from[i][0] = (int16_t)(w / 2 + (frand() - 0.5f) * 40);   // os pixels saem do cubo
        s_from[i][1] = (int16_t)(h * 0.36f + (frand() - 0.5f) * 40);
        s_delay[i] = (uint8_t)(frand() * 40);
    }
}

void anim_about_draw(uint32_t *buf, int stride, float t, float tilt)
{
    B = buf;
    BS = stride;
    fill(0x000000);
    int cx = BW / 2, cy = BH * 36 / 100;
    const float JUMP = 1.7f;
    if (t < JUMP) {
        // salto: as estrelas esticam-se em riscos a partir do centro, cada vez mais depressa
        float acc = t * t * 1.4f;
        for (int i = 0; i < NWARP; i++) {
            float a = s_warp[i][0], d0 = s_warp[i][1], sp = s_warp[i][2];
            float d = fmodf(d0 + acc * 260 * sp, 360);   // em ciclo: as estrelas continuam a passar
            float len = 4 + acc * 120 * sp;
            float cs = cosf(a), sn = sinf(a);
            uint32_t c = i % 5 == 0 ? 0xA8C8FF : i % 7 == 0 ? 0xD2A8FF : 0xFFFFFF;
            line_a(BW / 2 + cs * d, BH / 2 + sn * d, BW / 2 + cs * (d + len), BH / 2 + sn * (d + len), c,
                   (int)(80 + 176 * clampf(t / 0.8f)));
        }
        return;
    }
    float k = t - JUMP;
    stars_draw(t, (int)(150 * clampf(k / 0.6f)));
    const vox_model_t *cube = vox_get("deck3");
    float ft = vox_fly_time(cube);
    if (k > ft)
        for (int j = 0; j < 3; j++) {
            float r = fmodf((k - ft) * 70 + j * 50, 150);
            ring(cx, cy, 64 + (int)r, PAL[(j * 3 + 1) % 8], (int)(clampf(1 - r / 150) * 120));
        }
    vox_view_t v = VOX_VIEW_DEFAULT;
    v.scale = 30;
    v.cx = cx;
    v.cy = cy;
    v.yaw = 0.785f + k * 0.6f;
    v.pitch = 0.6f + tilt;
    v.fly = k < ft + 0.1f ? k + 1 : 0;
    vox_render(cube, &v, buf, BW, BH, stride);
    // o nome sai do cubo
    for (int i = 0; i < s_ncells; i++) {
        float e = ease((k - ft * 0.7f - s_delay[i] / 100.0f) / 1.0f);
        if (e <= 0) continue;
        int x = (int)lroundf(s_from[i][0] + (s_cells[i][0] - s_from[i][0]) * e);
        int y = (int)lroundf(s_from[i][1] + (s_cells[i][1] - s_from[i][1]) * e);
        uint32_t c = s_cells[i][2] & SUP ? PAL[6] : PAL[((s_cells[i][2] & 0xFF) * 2) % 8];
        int z = cell_sz(8, s_cells[i][2]) - 1;
        rect(x, y, z, z, e < 1 ? mix(0, c, (int)(100 + 156 * e)) : c);
    }
    // clarão no fim do salto
    if (k < 0.35f) {
        int a = (int)((1 - k / 0.35f) * 230);
        for (int y = 0; y < BH; y += 1)
            for (int x = 0; x < BW; x++) blend(x, y, 0xFFFFFF, a);
    }
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
    // o cubo pequeno a rodar devagar
    vox_view_t v = VOX_VIEW_DEFAULT;
    v.scale = 15;
    v.cx = cx;
    v.cy = 62 + dy;
    v.yaw = 0.785f + t * 0.25f;
    vox_render(vox_get("deck3"), &v, buf, w, h, stride);
    // hora grande, cada dígito com a sua cor da paleta
    int sc = 14, tw = pfont_width(a->time, sc), x = cx - tw / 2, y = 120 + dy;
    char one[2] = {0, 0};
    for (const char *p = a->time; *p; p++) {
        one[0] = *p;
        uint32_t c = *p == ':' ? ((int)(t * 2) % 2 ? 0x9AA4C0 : 0x3A4460) : PAL[((p - a->time) * 2) % 8];
        pfont_draw(buf, w, h, stride, one, x, y, sc, c, 0x0E1020, -1);
        x += pfont_width(one, sc) + sc;
    }
    B = buf; BS = stride;
    pfont_draw(buf, w, h, stride, a->date, cx - pfont_width(a->date, 4) / 2, y + 7 * sc + 20, 4, 0xB8C2D0, 0, -1);
    // bateria: barra em blocos de voxel (10 blocos) com a percentagem
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
// Faces do relógio do uConsole no cubo (tpl "clock"). A aurora é a cena "Sobre" do DashDeck: desenha-se a
// meia resolução (240x240, pixel art como no C6) e amplia-se 2x; a hora, a data e o tempo vão por cima em
// resolução total.

#define LO 240
static uint32_t *s_lo;
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
    // céu em faixas com pontilhado
    for (int y = 0; y < HZ; y++) {
        int f = y * 12 * 256 / HZ, b = f >> 8, fr = f & 255;
        uint32_t c0 = mix(0x050818, 0x1A1440, b * 256 / 12), c1 = mix(0x050818, 0x1A1440, (b + 1) * 256 / 12);
        uint32_t *row = &B[y * BS];
        for (int x = 0; x < LO; x++) row[x] = 0xFF000000u | ((fr > 128 && ((x + y) & 1) == 0) ? c1 : c0);
    }
    for (int i = 0; i < 70; i++)
        blend(s_cstar[i][0], s_cstar[i][1], 0xFFFFF0, (int)(140 + 100 * sinf(t * 2.3f + s_cstar[i][2] / 100.0f)));
    // aurora: cortinas verticais que ondulam, mais intensas em baixo
    for (int x = 0; x < LO; x++) {
        float fx = (float)x * 1.333f;   // a mesma "largura" de ondas da cena do DashDeck (320 px)
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
    // estrela cadente a cada 4 s
    float ph = fmodf(t, 4.0f);
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
    // montanhas com um toque de verde da aurora no topo
    for (int x = 0; x < LO; x++) {   // longe: azul escuro com neve no cimo; perto: colinas quase pretas
        int fh = s_cfar[x], mh = s_cmid[x];
        for (int y = fh; y < HZ; y++) {
            uint32_t c = y >= mh ? 0x070D10 : mix(0x0C1030, 0x080A1E, (y - fh) * 256 / (HZ - fh + 1));
            if (y < mh && y < fh + 5 && (x * 7 + y) % 5) c = mix(c, 0x8FA6C8, 120);   // neve
            B[y * BS + x] = 0xFF000000u | c;
        }
        put(x, fh, 0x4FA89A);   // o verde da aurora no cimo
    }
    // cabana na margem, janela acesa e fumo
    int cx = LO * 20 / 100, cy = HZ - 2;
    rect(cx - 8, cy - 9, 16, 9, 0x2A1A12);
    for (int i = 0; i < 10; i++) rect(cx - 10 + i, cy - 9 - i / 2, 20 - 2 * i, 1, 0x3A2416);
    rect(cx + 2, cy - 6, 4, 3, ((int)(t * 3) % 7) ? 0xFFC860 : 0xFFE0A0);
    for (int i = 0; i < 6; i++) {
        float q = fmodf(t * 0.5f + i / 6.0f, 1);
        blend(cx + 5 + (int)(4 * sinf(q * 6 + t)), cy - 15 - (int)(q * 26), 0x9098B0, (int)(120 * (1 - q)));
    }
    // lago: reflexo ondulado e mais escuro
    int phs = (int)(t * 3 * 256);
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

// ícone do tempo (16x16, paleta) em blocos de sc píxeis
static void wx_icon(int x, int y, int sc, const clock3_t *c)
{
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            int k = c->icon[j * 16 + i];
            if (k > 0 && k < 8) rect(x + i * sc, y + j * sc, sc, sc, c->icon_pal[k]);
        }
}

const char *anim_clock_name(int face)
{
    static const char *N[ANIM_CLOCK_FACES] = {"AURORA", "CUBO"};
    return N[((face % ANIM_CLOCK_FACES) + ANIM_CLOCK_FACES) % ANIM_CLOCK_FACES];
}

void anim_clock_draw(int face, uint32_t *buf, int w, int h, int stride, float t, const clock3_t *c)
{
    face = ((face % ANIM_CLOCK_FACES) + ANIM_CLOCK_FACES) % ANIM_CLOCK_FACES;
    char wx[40] = "";
    if (c->has_wx) snprintf(wx, sizeof(wx), "%d\xC2\xB0  MAX %d  MIN %d  CHUVA %d%%", c->temp, c->tmax, c->tmin, c->rain);
    if (face == 1) {   // o cubo a rodar, a hora às cores, a bateria e o tempo
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
    aurora_scene(t);
    up2(buf, w, h, stride);
    B = buf; BW = w; BH = h; BS = stride;
    int dx = (c->shift % 3 - 1) * 6, dy = (c->shift / 3 - 1) * 5;   // desloca-se um pouco a cada minuto (AMOLED)
    // hora grande sobre o céu, os dois pontos a piscar
    int sc = 13, tw = pfont_width(c->time, sc), x = w / 2 - tw / 2 + dx, y = 34 + dy;
    char one[2] = {0, 0};
    for (const char *p = c->time; *p; p++) {
        one[0] = *p;
        uint32_t col = *p == ':' ? ((int)(t * 2) % 2 ? 0xBFFFE8 : 0x2E6A60) : 0xEFFFF8;
        pfont_draw(buf, w, h, stride, one, x, y, sc, col, 0x07161C, -1);
        x += pfont_width(one, sc) + sc;
    }
    B = buf; BW = w; BH = h; BS = stride;
    pfont_draw(buf, w, h, stride, c->date, w / 2 - pfont_width(c->date, 4) / 2 + dx, y + 7 * sc + 16, 4, 0xB8E8DA,
               0x07161C, -1);
    B = buf; BW = w; BH = h; BS = stride;
    // tempo, em baixo sobre o lago: ícone, temperatura e máxima/mínima/chuva
    if (c->has_wx) {
        int bx = 34 + dx, by = h - 118 + dy;
        if (c->has_icon) wx_icon(bx, by, 5, c);
        char tt[12];
        snprintf(tt, sizeof(tt), "%d\xC2\xB0", c->temp);
        pfont_draw(buf, w, h, stride, tt, bx + 96, by + 6, 7, 0xFFFFFF, 0x07161C, -1);
        B = buf; BW = w; BH = h; BS = stride;
        char mm[32];
        snprintf(mm, sizeof(mm), "%d / %d  %d%%", c->tmax, c->tmin, c->rain);
        pfont_draw(buf, w, h, stride, mm, bx + 98, by + 66, 3, 0x9FD8FF, 0x07161C, -1);
        B = buf; BW = w; BH = h; BS = stride;
    }
}
