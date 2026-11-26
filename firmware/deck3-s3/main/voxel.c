#include "voxel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sd.h"

static const char *TAG = "vox";

// --------------------------------------------------------------------------------------- modelos

static inline uint8_t *cell(vox_model_t *m, int x, int y, int z)
{
    return &m->cells[(z * VOX_MAX + y) * VOX_MAX + x];
}

static inline uint8_t cell_get(const vox_model_t *m, int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= m->nx || y >= m->ny || z >= m->nz) {
        return 0;
    }
    return m->cells[(z * VOX_MAX + y) * VOX_MAX + x];
}

static int hexval(char c)
{
    if (c >= '1' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// Modelo embutido em "mapa de alturas": cada célula (x,y) tem uma cor e uma altura (coluna de voxels).
// Chega para quase todos os ícones e fica legível no código. Linha 0 = fundo (longe da câmara).
typedef struct {
    const char *name;
    uint8_t w, d;
    const char *colors;    // w*d caracteres (sem separadores)
    const char *heights;   // w*d dígitos '0'..'9'
    uint32_t pal[8];
} vox_hmap_t;

static const vox_hmap_t HMAPS[] = {
    // bloco genérico (a cor 1 é tingida com a cor do item)
    {"block", 4, 4, "1111" "1111" "1111" "1111", "2332" "3333" "3333" "2332", {0, 0x3EC6FF}},
    // barras (Stocks, sistema)
    {"chart", 7, 2, "1.2.3.4" "1.2.3.4", "2040306" "2040306", {0, 0x3EC6FF, 0x9B6BFF, 0xFFB23E, 0x4BE38A}},
    // avião visto de cima (ADS-B)
    {"plane", 7, 7,
     "...1..." "...1..." "1111111" ".11111." "...1..." "..111.." "...2...",
     "...2..." "...2..." "1112111" ".11211." "...2..." "..121.." "...3...", {0, 0xE8F0FF, 0xFF6B6B}},
    // navio (AIS)
    {"ship", 7, 3, ".11211." "1112111" ".11111.", "0123210" "1135311" "0122210", {0, 0x2F6DB5, 0xE8F0FF, 0xFFB23E}},
    // torre de rádio (Meshtastic, SDR)
    {"radio", 5, 5, "..2.." ".313." "21112" ".313." "..2..", "00100" "01310" "13813" "01310" "00100",
     {0, 0x9B6BFF, 0x4BE38A, 0x6B4BD8}},
    // marcador de posição (GPS)
    {"gps", 5, 5, ".111." "11211" "12221" "11211" ".111.", "02320" "24542" "35753" "24542" "02320",
     {0, 0xFF5470, 0xFFD0D8}},
    // escadas do Wi-Fi (rede)
    {"wifi", 5, 4, "11111" "22222" "33333" "..4..", "11111" "22222" "33333" "00400", {0, 0x2F6DB5, 0x3E9BFF, 0x3EC6FF, 0xE8F0FF}},
    // engrenagem (sistema, definições)
    {"gear", 6, 6, ".1..1." "111111" ".1..1." ".1..1." "111111" ".1..1.", "020020" "222222" "020020" "020020" "222222" "020020",
     {0, 0x9AA7B8}},
    // relógio: disco com ponteiros
    {"clock", 7, 7,
     "..111.." ".11111." "1112111" "1112221" "1111111" ".11111." "..111..",
     "..111.." ".11111." "1112111" "1112221" "1111111" ".11111." "..111..", {0, 0x3A5068, 0xFFB23E}},
    // aplicações: 4 cubos
    {"apps", 5, 5, "11.22" "11.22" "....." "33.44" "33.44", "22022" "22022" "00000" "22022" "22022",
     {0, 0x3EC6FF, 0xFF5470, 0x4BE38A, 0xFFB23E}},
    // câmara
    {"cam", 6, 4, "111111" "112211" "112211" "111111", "333333" "334433" "334433" "333333", {0, 0x6B7686, 0x3EC6FF}},
    // bateria / energia
    {"power", 6, 3, "111112" "111112" "111112", "333331" "333331" "333331", {0, 0x4BE38A, 0x9AA7B8}},
    // envelope (avisos de mensagem)
    {"mail", 6, 4, "111111" "121121" "112211" "111111", "222222" "232232" "223322" "222222", {0, 0xE8F0FF, 0x3EC6FF}},
    // chip (sistema)
    {"chip", 6, 6, ".2.2.." "211112" ".1331." ".1331." "211112" "..2.2.", "010100" "122221" "023320" "023320" "122221" "001010",
     {0, 0x46566B, 0xC4CEDB, 0x3EC6FF}},
    // nota musical (som)
    {"audio", 5, 3, "1...." "1..1." "11.11", "70000" "70070" "11011", {0, 0xFF5470}},
    // casa (início)
    {"home", 5, 5, ".222." "21112" "21312" "21112" ".222.", "03330" "35553" "35253" "35553" "03330", {0, 0xE8F0FF, 0xFF6B6B, 0x6B4BD8}},
};

#define MAX_USER 24
static vox_model_t *s_user[MAX_USER];
static char s_user_id[MAX_USER][24];
static vox_model_t *s_builtin[sizeof(HMAPS) / sizeof(HMAPS[0])];
static vox_model_t *s_deck3;

static vox_model_t *new_model(void)
{
    vox_model_t *m = heap_caps_calloc(1, sizeof(vox_model_t), MALLOC_CAP_SPIRAM);
    return m;
}

static vox_model_t *from_hmap(const vox_hmap_t *h)
{
    vox_model_t *m = new_model();
    if (!m) return NULL;
    int maxh = 1;
    for (int i = 0; i < h->w * h->d; i++) {
        int v = h->heights[i] - '0';
        if (v > maxh) maxh = v;
    }
    m->nx = h->w;
    m->ny = h->d;
    m->nz = maxh > VOX_MAX ? VOX_MAX : maxh;
    memcpy(m->pal, h->pal, sizeof(h->pal));
    for (int y = 0; y < h->d; y++) {
        for (int x = 0; x < h->w; x++) {
            int i = y * h->w + x;
            int c = hexval(h->colors[i]);
            int hh = h->heights[i] - '0';
            for (int z = 0; z < hh && z < m->nz; z++) {
                *cell(m, x, y, z) = c;
            }
        }
    }
    return m;
}

// O símbolo do sistema: cubo 3×3×3 (Deck elevado a 3), cada voxel com uma das cores da marca.
static vox_model_t *make_deck3(void)
{
    vox_model_t *m = new_model();
    if (!m) return NULL;
    m->nx = m->ny = m->nz = 3;
    m->pal[1] = 0x3EC6FF;   // ciano
    m->pal[2] = 0x9B6BFF;   // violeta
    m->pal[3] = 0xFFB23E;   // âmbar
    m->pal[4] = 0xE8F0FF;   // branco (centro de cada face)
    for (int z = 0; z < 3; z++) {
        for (int y = 0; y < 3; y++) {
            for (int x = 0; x < 3; x++) {
                int centers = (x == 1) + (y == 1) + (z == 1);
                *cell(m, x, y, z) = centers == 2 ? 4 : 1 + (x + y + z) % 3;
            }
        }
    }
    return m;
}

// Modelos guardados no cartão: /sdcard/deck3/vox/<nome>.json (o mesmo formato do {"t":"vox"}).
// Os nomes que não existem ficam lembrados para não ir ao cartão a cada desenho.
#define MAX_MISS 16
static char s_miss[MAX_MISS][24];
static int s_nmiss;
bool vox_define(const cJSON *msg);

static const vox_model_t *load_sd(const char *name)
{
    if (!sd_ok() || strlen(name) > 23 || strchr(name, '/')) return NULL;
    for (int i = 0; i < s_nmiss && i < MAX_MISS; i++)
        if (!strcmp(s_miss[i], name)) return NULL;
    char rel[48];
    snprintf(rel, sizeof(rel), "vox/%s.json", name);
    char *txt = sd_read_text(rel, 65536);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    heap_caps_free(txt);
    if (j) {
        if (!cJSON_GetObjectItem(j, "id")) cJSON_AddStringToObject(j, "id", name);
        else cJSON_ReplaceItemInObject(j, "id", cJSON_CreateString(name));
        bool ok = vox_define(j);
        cJSON_Delete(j);
        if (ok) {
            for (int i = 0; i < MAX_USER; i++)
                if (s_user[i] && strcmp(s_user_id[i], name) == 0) return s_user[i];
        }
    }
    strlcpy(s_miss[s_nmiss % MAX_MISS], name, sizeof(s_miss[0]));
    s_nmiss++;
    return NULL;
}

void vox_forget_missing(void)
{
    s_nmiss = 0;
}

const vox_model_t *vox_get(const char *name)
{
    if (!name || !*name) return NULL;
    if (strncmp(name, "vox:", 4) == 0) name += 4;
    if (strcmp(name, "deck3") == 0) {
        if (!s_deck3) s_deck3 = make_deck3();
        return s_deck3;
    }
    for (int i = 0; i < MAX_USER; i++) {
        if (s_user[i] && strcmp(s_user_id[i], name) == 0) return s_user[i];
    }
    for (size_t i = 0; i < sizeof(HMAPS) / sizeof(HMAPS[0]); i++) {
        if (strcmp(HMAPS[i].name, name) == 0) {
            if (!s_builtin[i]) s_builtin[i] = from_hmap(&HMAPS[i]);
            return s_builtin[i];
        }
    }
    return load_sd(name);
}

static uint32_t parse_color(const char *s)
{
    if (!s) return 0;
    if (*s == '#') s++;
    return (uint32_t)strtoul(s, NULL, 16) & 0xFFFFFF;
}

bool vox_define(const cJSON *msg)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
    const cJSON *size = cJSON_GetObjectItem(msg, "size");
    const cJSON *layers = cJSON_GetObjectItem(msg, "layers");
    if (!id || !cJSON_IsArray(size) || cJSON_GetArraySize(size) != 3 || !cJSON_IsArray(layers)) {
        return false;
    }
    int nx = cJSON_GetArrayItem(size, 0)->valueint, ny = cJSON_GetArrayItem(size, 1)->valueint,
        nz = cJSON_GetArrayItem(size, 2)->valueint;
    if (nx < 1 || ny < 1 || nz < 1 || nx > VOX_MAX || ny > VOX_MAX || nz > VOX_MAX) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < MAX_USER; i++) {
        if (s_user[i] && strcmp(s_user_id[i], id) == 0) { slot = i; break; }
        if (!s_user[i] && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;   // cheio: substitui o primeiro
    if (!s_user[slot]) s_user[slot] = new_model();
    vox_model_t *m = s_user[slot];
    if (!m) return false;
    memset(m, 0, sizeof(*m));
    strlcpy(s_user_id[slot], id, sizeof(s_user_id[slot]));
    m->nx = nx; m->ny = ny; m->nz = nz;
    const cJSON *pal = cJSON_GetObjectItem(msg, "pal");
    for (int i = 0; i < 15 && i < cJSON_GetArraySize(pal); i++) {
        m->pal[i + 1] = parse_color(cJSON_GetStringValue(cJSON_GetArrayItem(pal, i)));
    }
    for (int z = 0; z < nz && z < cJSON_GetArraySize(layers); z++) {
        const cJSON *rows = cJSON_GetArrayItem(layers, z);
        for (int y = 0; y < ny && y < cJSON_GetArraySize(rows); y++) {
            const char *r = cJSON_GetStringValue(cJSON_GetArrayItem(rows, y));
            for (int x = 0; r && x < nx && r[x]; x++) {
                *cell(m, x, y, z) = hexval(r[x]);
            }
        }
    }
    return true;
}

const char *vox_for_icon(const char *icon)
{
    static const char *map[][2] = {
        {"gps", "gps"}, {"wifi", "wifi"}, {"settings", "gear"}, {"bars", "chart"}, {"home", "home"},
        {"audio", "audio"}, {"volume", "audio"}, {"video", "cam"}, {"image", "cam"}, {"battery", "power"},
        {"charge", "power"}, {"power", "power"}, {"mail", "mail"}, {"bell", "mail"}, {"list", "apps"},
        {"folder", "apps"}, {"drive", "chip"}, {"sd", "chip"}, {"usb", "chip"}, {"call", "radio"},
        {"bluetooth", "radio"}, {"eye", "cam"},
    };
    if (!icon) return "block";
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i][0], icon) == 0) return map[i][1];
    }
    return vox_get(icon) ? icon : "block";
}

// ------------------------------------------------------------------------------------ renderização

typedef struct {
    float depth;
    float x[4], y[4];
    uint32_t color;
} face_t;

static int face_cmp(const void *a, const void *b)
{
    float d = ((const face_t *)a)->depth - ((const face_t *)b)->depth;
    return d < 0 ? -1 : d > 0;
}

static inline uint32_t shade(uint32_t rgb, float k)
{
    int r = (int)(((rgb >> 16) & 0xFF) * k), g = (int)(((rgb >> 8) & 0xFF) * k), b = (int)((rgb & 0xFF) * k);
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void fill_tri(uint32_t *buf, int w, int h, int stride, const float *x, const float *y, int a, int b, int c,
                     uint32_t col)
{
    float area = edge(x[a], y[a], x[b], y[b], x[c], y[c]);
    if (fabsf(area) < 0.01f) return;
    if (area < 0) { int t = b; b = c; c = t; }
    int x0 = (int)floorf(fminf(x[a], fminf(x[b], x[c]))), x1 = (int)ceilf(fmaxf(x[a], fmaxf(x[b], x[c])));
    int y0 = (int)floorf(fminf(y[a], fminf(y[b], y[c]))), y1 = (int)ceilf(fmaxf(y[a], fmaxf(y[b], y[c])));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = buf + py * stride;
        float fy = py + 0.5f;
        for (int px = x0; px < x1; px++) {
            float fx = px + 0.5f;
            if (edge(x[a], y[a], x[b], y[b], fx, fy) >= 0 && edge(x[b], y[b], x[c], y[c], fx, fy) >= 0 &&
                edge(x[c], y[c], x[a], y[a], fx, fy) >= 0) {
                row[px] = col;
            }
        }
    }
}

static void fill_quad(uint32_t *buf, int w, int h, int stride, const float *x, const float *y, uint32_t col)
{
    fill_tri(buf, w, h, stride, x, y, 0, 1, 2, col);
    fill_tri(buf, w, h, stride, x, y, 0, 2, 3, col);
}

// Normais e cantos (no cubo unitário) das 6 faces, na ordem do contorno.
static const int8_t NRM[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static const int8_t CRN[6][4][3] = {
    {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
    {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},
};

// Montagem "fly": atraso de cada voxel (camada z primeiro, depois ao acaso) e tempo de voo.
#define FLY_LAYER 0.16f
#define FLY_JITTER 0.035f
#define FLY_DUR 0.75f

float vox_fly_time(const vox_model_t *m)
{
    return m ? m->nz * FLY_LAYER + 12 * FLY_JITTER + FLY_DUR : 0;
}

static float ease_out3(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    float u = 1 - t;
    return 1 - u * u * u;
}

void vox_render(const vox_model_t *m, const vox_view_t *v, uint32_t *buf, int w, int h, int stride)
{
    if (!m || !buf) return;
    float cy_ = cosf(v->yaw), sy_ = sinf(v->yaw), cp = cosf(v->pitch), sp = sinf(v->pitch);
    float scale = v->scale;
    float mx = m->nx / 2.0f, my = m->ny / 2.0f, mz = m->nz / 2.0f;
    float ox = v->cx ? v->cx : w / 2.0f, oy = v->cy ? v->cy : h / 2.0f;
    if (scale <= 0) {
        // ajusta pela caixa projetada (não pela diagonal 3D): modelos achatados ficam do tamanho certo
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < 8; k++) {
            float wx = (k & 1 ? m->nx : 0) - mx, wy = (k & 2 ? m->ny : 0) - my, wz = (k & 4 ? m->nz : 0) - mz;
            float sx = wx * cy_ - wy * sy_, sy = (wx * sy_ + wy * cy_) * sp - wz * cp;
            x0 = fminf(x0, sx); x1 = fmaxf(x1, sx); y0 = fminf(y0, sy); y1 = fmaxf(y1, sy);
        }
        scale = fminf(w * 0.9f / (x1 - x0), h * 0.9f / (y1 - y0));
        ox -= (x0 + x1) / 2 * scale;   // centra a caixa
        oy -= (y0 + y1) / 2 * scale;
    }
    // luz fixa na câmara (vem de cima, à esquerda, da frente)
    const float L[3] = {-0.35f, 0.55f, 0.76f};

    // visibilidade e luz de cada uma das 6 normais (iguais para todos os voxels)
    bool vis[6];
    float lum[6];
    for (int f = 0; f < 6; f++) {
        float nx = NRM[f][0] * cy_ - NRM[f][1] * sy_, ny = NRM[f][0] * sy_ + NRM[f][1] * cy_, nz = NRM[f][2];
        float depth = ny * cp + nz * sp;          // para a câmara
        float up = nz * cp - ny * sp;              // para cima no ecrã
        vis[f] = depth > 0.001f;
        float d = nx * L[0] + up * L[1] + depth * L[2];
        lum[f] = 0.42f + 0.62f * (d > 0 ? d : 0);
    }

    // montagem acabada: volta a esconder as faces interiores (bem mais rápido)
    bool flying_any = v->fly > 0 && v->fly - 1 < vox_fly_time(m);
    int nfaces = 0, cap = 0;
    for (int z = 0; z < m->nz; z++)   // 3 faces visíveis por voxel no máximo
        for (int y = 0; y < m->ny; y++)
            for (int x = 0; x < m->nx; x++)
                if (cell_get(m, x, y, z)) cap += 3;
    if (!cap) return;
    face_t *faces = heap_caps_malloc(sizeof(face_t) * cap, MALLOC_CAP_SPIRAM);
    if (!faces) {
        ESP_LOGW(TAG, "sem memória para %d faces", cap);
        return;
    }
    for (int z = 0; z < m->nz; z++) {
        for (int y = 0; y < m->ny; y++) {
            for (int x = 0; x < m->nx; x++) {
                int ci = cell_get(m, x, y, z);
                if (!ci) continue;
                uint32_t base = (ci == 1 && v->tint) ? v->tint : m->pal[ci];
                // textura: variação pequena e estável por voxel
                uint32_t hsh = (uint32_t)(x * 73856093u) ^ (uint32_t)(y * 19349663u) ^ (uint32_t)(z * 83492791u);
                float var = 0.94f + (hsh % 13) * 0.01f;
                // posição (com explosão para a animação de montagem)
                float px = x - mx, py = y - my, pz = z - mz;
                float flying = 0;   // 1 = acabou de partir, 0 = no lugar
                if (v->fly > 0) {
                    float t = v->fly - 1 - (z * FLY_LAYER + (hsh % 13) * FLY_JITTER);
                    if (t < 0) continue;                       // ainda não partiu
                    float e = ease_out3(t / FLY_DUR);
                    flying = 1 - e;
                    // parte de cima e de lado (direção estável por voxel), como os pixels do DashDeck
                    px += flying * ((int)((hsh >> 4) % 21) - 10) * 0.9f;
                    py += flying * ((int)((hsh >> 9) % 21) - 10) * 0.9f;
                    pz += flying * (7 + (hsh >> 14) % 7);
                }
                if (v->explode > 0) {
                    float k = 1 + v->explode * 2.5f;
                    px = (px + 0.5f) * k - 0.5f;
                    py = (py + 0.5f) * k - 0.5f;
                    pz = (pz + 0.5f) * k - 0.5f + v->explode * ((hsh % 7) - 3) * 0.6f;
                }
                for (int f = 0; f < 6 && nfaces < cap; f++) {
                    if (!vis[f]) continue;
                    if (v->explode <= 0 && !flying_any &&
                        cell_get(m, x + NRM[f][0], y + NRM[f][1], z + NRM[f][2])) continue;
                    face_t *fc = &faces[nfaces];
                    float dsum = 0;
                    for (int k = 0; k < 4; k++) {
                        float wx = px + CRN[f][k][0], wy = py + CRN[f][k][1], wz = pz + CRN[f][k][2];
                        float rx = wx * cy_ - wy * sy_, ry = wx * sy_ + wy * cy_;
                        fc->x[k] = ox + rx * scale;
                        fc->y[k] = oy + (ry * sp - wz * cp) * scale;
                        dsum += ry * cp + wz * sp;
                    }
                    fc->depth = dsum / 4;
                    // a voar: mais claro (como os pixels do DashDeck, que chegam do tom claro ao final)
                    fc->color = shade(base, lum[f] * var * (1 + flying * 0.7f + v->glow * 0.35f));
                    nfaces++;
                }
            }
        }
    }
    qsort(faces, nfaces, sizeof(face_t), face_cmp);
    for (int i = 0; i < nfaces; i++) {
        face_t *fc = &faces[i];
        // face inteira um pouco mais escura (aresta) + interior na cor da face (bisel)
        if (scale < 5) {
            fill_quad(buf, w, h, stride, fc->x, fc->y, fc->color);
        } else {
            fill_quad(buf, w, h, stride, fc->x, fc->y, shade(fc->color & 0xFFFFFF, 0.72f));
            float cx = (fc->x[0] + fc->x[1] + fc->x[2] + fc->x[3]) / 4;
            float cyy = (fc->y[0] + fc->y[1] + fc->y[2] + fc->y[3]) / 4;
            float ix[4], iy[4], k = scale >= 14 ? 0.84f : 0.78f;
            for (int j = 0; j < 4; j++) {
                ix[j] = cx + (fc->x[j] - cx) * k;
                iy[j] = cyy + (fc->y[j] - cyy) * k;
            }
            fill_quad(buf, w, h, stride, ix, iy, fc->color);
        }
    }
    heap_caps_free(faces);
}
