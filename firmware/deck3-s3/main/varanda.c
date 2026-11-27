// Varanda do Romeu (ver varanda.h). Formatos no cartão (escritos pelo dashd, host/varanda.py):
//   varanda/bg.bin      "VBG1", u16 w, u16 h, paleta 256 x RGB888, w*h índices
//   varanda/<nome>.bin  "VSP1", u16 w, h, ax, ay, w*h RGB565 (LE), w*h alfa (0/1)
//   varanda/poses.json  {"sit":["c0_17"], "walk":[...], ...}
// O fundo desenha-se pela paleta do modo (dia, entardecer, noite, chuva): a noite é uma troca de paleta.
#include "varanda.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "anim.h"
#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "imu.h"
#include "sd.h"

static const char *TAG = "varanda";
#define W VAR_W
#define H VAR_H

// ------------------------------------------------------------------------------------------ imagens
typedef struct {
    uint16_t w, h, ax, ay;
    uint16_t *px;
    uint8_t *a;
} spr_t;

enum { P_SIT, P_WAG, P_FRONT, P_BARK, P_ALERT, P_UP, P_SCARED, P_YAWN, P_SHAKE, P_SPIN, P_WALK, P_RAIL, P_LIE,
       P_SLEEP, P_PANT, P_JUMP, P_ENV, P_PARTY, P_DRINK, NPOSE };
static const char *POSE_NAME[NPOSE] = {"sit", "wag", "front", "bark", "alert", "up", "scared", "yawn", "shake",
                                       "spin", "walk", "rail", "lie", "sleep", "pant", "jump", "env", "party",
                                       "drink"};
#define MAXSPR 48
#define MAXF 4
static spr_t s_spr[MAXSPR];
static char s_sprname[MAXSPR][16];
static int s_nspr;
static int8_t s_pose[NPOSE][MAXF];   // índices em s_spr; -1 = nenhum
static int s_nfr[NPOSE];
static int s_bowl = -1;
static uint8_t *s_bg;                // índices
static uint8_t *s_bgraw, *s_sprraw[MAXSPR];
static uint8_t s_pal[256][3];
static uint16_t s_lut[4][256];       // paleta por modo, já em RGB565
static uint8_t *s_out;               // 1 = céu ou mar (onde aparecem nuvens, aviões, barcos, chuva)
static bool s_loaded;

enum { M_DAY, M_DUSK, M_NIGHT, M_STORM };

static void mode_rgb(int m, int *r, int *g, int *b)
{
    float R = *r, G = *g, B = *b;
    if (m == M_DUSK) { R = R * 1.1f + 18; G = G * 0.86f + 4; B = B * 0.72f; }
    else if (m == M_NIGHT) { R = R * 0.2f + 4; G = G * 0.24f + 6; B = B * 0.42f + 16; }
    else if (m == M_STORM) {
        float l = (R + G + B) / 3;
        R = (R * 0.55f + l * 0.45f) * 0.62f; G = (G * 0.55f + l * 0.45f) * 0.66f; B = (B * 0.55f + l * 0.45f) * 0.78f;
    }
    *r = R > 255 ? 255 : R < 0 ? 0 : (int)R;
    *g = G > 255 ? 255 : G < 0 ? 0 : (int)G;
    *b = B > 255 ? 255 : B < 0 ? 0 : (int)B;
}

static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint8_t *read_file(const char *rel, size_t *len)
{
    char p[96];
    if (!sd_path(rel, p, sizeof(p))) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) : NULL;
    if (d && fread(d, 1, n, f) != (size_t)n) {
        heap_caps_free(d);
        d = NULL;
    }
    fclose(f);
    if (d) d[n] = 0;
    if (len) *len = d ? (size_t)n : 0;
    return d;
}

static int spr_load(const char *name)
{
    for (int i = 0; i < s_nspr; i++)
        if (!strcmp(s_sprname[i], name)) return i;
    if (s_nspr >= MAXSPR) return -1;
    char rel[48];
    snprintf(rel, sizeof(rel), "varanda/%s.bin", name);
    size_t n;
    uint8_t *d = read_file(rel, &n);
    if (!d) return -1;
    uint16_t *hd = (uint16_t *)(d + 4);
    if (n < 12 || memcmp(d, "VSP1", 4) || n < 12 + (size_t)hd[0] * hd[1] * 3) {
        heap_caps_free(d);
        return -1;
    }
    spr_t *s = &s_spr[s_nspr];
    s->w = hd[0]; s->h = hd[1]; s->ax = hd[2]; s->ay = hd[3];
    s_sprraw[s_nspr] = d;
    s->px = (uint16_t *)(d + 12);
    s->a = d + 12 + s->w * s->h * 2;
    strlcpy(s_sprname[s_nspr], name, sizeof(s_sprname[0]));
    return s_nspr++;
}

bool varanda_loaded(void)
{
    return s_loaded;
}

void varanda_unload(void)
{
    s_loaded = false;
    heap_caps_free(s_bgraw);
    heap_caps_free(s_out);
    s_bgraw = s_bg = s_out = NULL;
    for (int i = 0; i < s_nspr; i++) heap_caps_free(s_sprraw[i]);
    s_nspr = 0;
    s_bowl = -1;
}

bool varanda_load(void)
{
    if (s_loaded) return true;
    size_t n;
    uint8_t *bg = read_file("varanda/bg.bin", &n);
    if (!bg || n < 8 + 768 + W * H || memcmp(bg, "VBG1", 4)) {
        heap_caps_free(bg);
        return false;
    }
    s_bgraw = bg;
    memcpy(s_pal, bg + 8, 768);
    s_bg = bg + 8 + 768;
    for (int m = 0; m < 4; m++)
        for (int i = 0; i < 256; i++) {
            int r = s_pal[i][0], g = s_pal[i][1], b = s_pal[i][2];
            mode_rgb(m, &r, &g, &b);
            s_lut[m][i] = rgb565(r, g, b);
        }
    // máscara do céu e do mar: os azuis da paleta, acima do deck e entre as paredes
    s_out = heap_caps_malloc(W * H, MALLOC_CAP_SPIRAM);
    if (!s_out) return false;
    uint8_t blue[256];
    for (int i = 0; i < 256; i++) {
        int r = s_pal[i][0], g = s_pal[i][1], b = s_pal[i][2];
        blue[i] = b > 110 && b > r + 35 && b > g + 5;
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) s_out[y * W + x] = y < 345 && x > 40 && x < 420 && blue[s_bg[y * W + x]];
    // poses
    char *js = (char *)read_file("varanda/poses.json", NULL);
    cJSON *root = js ? cJSON_Parse(js) : NULL;
    heap_caps_free(js);
    if (!root) return false;
    for (int p = 0; p < NPOSE; p++) {
        s_nfr[p] = 0;
        const cJSON *arr = cJSON_GetObjectItem(root, POSE_NAME[p]), *it;
        cJSON_ArrayForEach(it, arr) {
            if (s_nfr[p] >= MAXF || !cJSON_IsString(it)) continue;
            int k = spr_load(it->valuestring);
            if (k >= 0) s_pose[p][s_nfr[p]++] = (int8_t)k;
        }
    }
    cJSON_Delete(root);
    s_bowl = spr_load("bowl");
    s_loaded = s_nfr[P_SIT] > 0 && s_nfr[P_WALK] > 0;
    ESP_LOGI(TAG, "varanda: %d sprites, %s", s_nspr, s_loaded ? "pronta" : "faltam poses");
    return s_loaded;
}

// ------------------------------------------------------------------------------------------ eventos
typedef struct {
    int type, a, b;
    char s1[24], s2[100];
} vev_t;
static QueueHandle_t s_q;

void varanda_event(int type, int a, int b, const char *s1, const char *s2)
{
    if (!s_q) s_q = xQueueCreate(12, sizeof(vev_t));
    vev_t e = {.type = type, .a = a, .b = b};
    if (s1) strlcpy(e.s1, s1, sizeof(e.s1));
    if (s2) strlcpy(e.s2, s2, sizeof(e.s2));
    xQueueSend(s_q, &e, 0);
}

// dados do dashd: guardados aqui (a tarefa de quadros lê-os no passo seguinte)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int wx;                     // 0 limpo, 1 chuva, 2 trovoada, 3 neve
    int sr, ss;                 // nascer e pôr do sol (minutos)
    char plane[16];
    int palt, pdist;            // m, 0,1 km
    char boat[24];
    int bdist;                  // 0,1 milha
    bool fresh_plane, fresh_boat;
    int force;                  // testes: modo forçado (-1 = pela hora)
} s_data = {.sr = 7 * 60, .ss = 18 * 60 + 30, .force = -1};

void varanda_event_json(const cJSON *msg)
{
    const char *wx = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "wx"));
    const char *fm = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "mode"));   // testes: day dusk night storm auto
    const cJSON *sun = cJSON_GetObjectItem(msg, "sun");
    const cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem(msg, "planes"), 0);
    const cJSON *bt = cJSON_GetArrayItem(cJSON_GetObjectItem(msg, "boats"), 0);
    taskENTER_CRITICAL(&s_mux);
    if (fm) s_data.force = !strcmp(fm, "day") ? M_DAY : !strcmp(fm, "dusk") ? M_DUSK : !strcmp(fm, "night") ? M_NIGHT : !strcmp(fm, "storm") ? M_STORM : -1;
    if (wx) s_data.wx = !strcmp(wx, "rain") ? 1 : !strcmp(wx, "storm") ? 2 : !strcmp(wx, "snow") ? 3 : 0;
    if (cJSON_GetArraySize(sun) == 2) {
        s_data.sr = cJSON_GetArrayItem(sun, 0)->valueint;
        s_data.ss = cJSON_GetArrayItem(sun, 1)->valueint;
    }
    const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(p, "id"));
    if (pid && strcmp(pid, s_data.plane)) {
        strlcpy(s_data.plane, pid, sizeof(s_data.plane));
        s_data.palt = cJSON_GetObjectItem(p, "alt") ? cJSON_GetObjectItem(p, "alt")->valueint : 0;
        s_data.pdist = (int)(cJSON_GetNumberValue(cJSON_GetObjectItem(p, "dist")) * 10);
        s_data.fresh_plane = true;
    }
    const char *bn = cJSON_GetStringValue(cJSON_GetObjectItem(bt, "name"));
    if (bn && strcmp(bn, s_data.boat)) {
        strlcpy(s_data.boat, bn, sizeof(s_data.boat));
        s_data.bdist = (int)(cJSON_GetNumberValue(cJSON_GetObjectItem(bt, "dist")) * 10);
        s_data.fresh_boat = true;
    }
    taskEXIT_CRITICAL(&s_mux);
}

// ------------------------------------------------------------------------------------------ o Romeu
typedef struct { int16_t x, y; } pt_t;
enum { S_DECK, S_LIE, S_RAIL, S_BED, S_BOWL, S_HIDE, S_ENV, NSPOT };
static const struct { int16_t x, y, wx, wy; uint8_t pose; } SPOT[NSPOT] = {
    [S_DECK] = {250, 442, 250, 442, P_SIT},
    [S_LIE] = {300, 446, 300, 446, P_LIE},
    [S_RAIL] = {96, 356, 96, 356, P_RAIL},
    [S_BED] = {226, 350, 226, 350, P_SLEEP},
    [S_BOWL] = {374, 432, 344, 432, P_DRINK},
    [S_HIDE] = {404, 326, 404, 326, P_SCARED},
    [S_ENV] = {160, 392, 160, 392, P_ENV},
};

static struct {
    float x, y;
    int dir;                    // 1 direita, -1 esquerda
    int spot, target;
    pt_t path[4];
    uint8_t hop[4];
    int npath;
    float speed, hop_t;
    int pose;                   // pose de base (a do lugar)
    int act;                    // ação temporária (-1 = nenhuma)
    float act_until, hold_until, idle_next, hearts_until;
    bool carry;                 // leva o envelope
} R;

static struct {
    float plane_t, boat_t, pigeon_t, msg_until, bar_until, light_t;
    bool plane_on, boat_on, pigeon_on, env_on, env_fetch;
    float env_fetch_t;
    char plane_lbl[32], boat_lbl[64], dm_from[24], dm_text[100];
    float px, py, bx;
    int wx;
    float mic_base, clap_t, loud_since;
    int clap_n;
} E;

static float s_t;   // instante do passo atual

static void go_to(int spot, float speed)
{
    int end_x = SPOT[spot].wx, end_y = SPOT[spot].wy, n = 0;
    if (R.spot == S_BED) { R.path[n] = (pt_t){204, 410}; R.hop[n++] = 1; }
    if (spot == S_BED) {
        R.path[n] = (pt_t){204, 410}; R.hop[n++] = 0;
        R.path[n] = (pt_t){SPOT[S_BED].x, SPOT[S_BED].y}; R.hop[n++] = 1;
    } else if (spot == S_RAIL || R.spot == S_RAIL) {
        R.path[n] = (pt_t){112, 424}; R.hop[n++] = 0;
        R.path[n] = (pt_t){end_x, end_y}; R.hop[n++] = 0;
    } else if (spot == S_HIDE || R.spot == S_HIDE) {
        R.path[n] = (pt_t){426, 410}; R.hop[n++] = 0;
        R.path[n] = (pt_t){end_x, end_y}; R.hop[n++] = 0;
    } else {
        R.path[n] = (pt_t){end_x, end_y}; R.hop[n++] = 0;
    }
    R.npath = n;
    R.target = spot;
    R.speed = speed > 0 ? speed : 60;
    R.hop_t = -1;
    R.act = -1;
}

static void act(int pose, float secs)
{
    if (s_nfr[pose] == 0) return;
    R.act = pose;
    R.act_until = s_t + secs;
}

static bool asleep(void)
{
    return R.npath == 0 && R.spot == S_BED;
}

static void wake(int mode_night)
{
    if (asleep() && !mode_night) go_to(S_DECK, 0);
}

void varanda_reset(void)
{
    memset(&R, 0, sizeof(R));
    memset(&E, 0, sizeof(E));
    R.x = SPOT[S_DECK].x;
    R.y = SPOT[S_DECK].y;
    R.dir = 1;
    R.spot = R.target = S_DECK;
    R.pose = P_SIT;
    R.act = -1;
    R.hop_t = -1;
    R.idle_next = 6;
    s_t = 0;
    vev_t e;
    while (s_q && xQueueReceive(s_q, &e, 0) == pdTRUE) {
    }
}

static int cur_mode(int *night_out)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min, sr, ss, wx, force;
    taskENTER_CRITICAL(&s_mux);
    sr = s_data.sr;
    ss = s_data.ss;
    wx = s_data.wx;
    force = s_data.force;
    taskEXIT_CRITICAL(&s_mux);
    E.wx = wx;
    if (force >= 0) {
        *night_out = force == M_NIGHT;
        if (force == M_STORM && !wx) E.wx = 1;
        return force;
    }
    bool night = tm.tm_year > 120 && (m < sr - 30 || m > ss + 40);
    *night_out = night;
    if (night) return M_NIGHT;
    if (wx == 1 || wx == 2) return M_STORM;
    if (tm.tm_year > 120 && m > ss - 50) return M_DUSK;
    return M_DAY;
}

static void handle_events(int night)
{
    vev_t e;
    while (s_q && xQueueReceive(s_q, &e, 0) == pdTRUE) {
        switch (e.type) {
        case VEV_TOUCH: {
            if (E.msg_until > s_t) { E.msg_until = 0; break; }
            if (E.env_on && fabsf(e.a - 130.0f) < 24 && fabsf(e.b - 380.0f) < 22) {
                if (asleep()) { E.env_on = false; E.msg_until = s_t + 7; E.bar_until = 0; }
                else go_to(S_ENV, 0);
                break;
            }
            if (fabsf(e.a - R.x) < 36 && e.b > R.y - 80 && e.b < R.y + 6) {
                R.hearts_until = s_t + 2.6f;
                if (!asleep() && R.npath == 0) act(P_WAG, 2.6f);
            }
            break;
        }
        case VEV_PET:
            R.hearts_until = s_t + 2.6f;
            if (!asleep() && R.npath == 0) act(P_WAG, 2.6f);
            break;
        case VEV_KNOCK:
            if (asleep()) { if (night) act(P_LIE, 2); else wake(0); }
            else if (R.npath == 0) act(P_BARK, 1.2f);
            break;
        case VEV_SHAKE:
            wake(night);
            if (!asleep() && R.npath == 0) act(P_SHAKE, 1.4f);
            break;
        case VEV_CLAP:
            wake(night);
            if (!asleep() && R.npath == 0) { act(P_SPIN, 1.6f); R.hearts_until = s_t + 2.4f; }
            break;
        case VEV_NOISE:
            if (!asleep()) { go_to(S_HIDE, 140); R.hold_until = s_t + 7; }
            break;
        case VEV_CHARGE:
            wake(night);
            if (!asleep()) { go_to(S_BOWL, 0); R.hold_until = s_t + 9; }
            break;
        case VEV_DM:
            strlcpy(E.dm_from, e.s1[0] ? e.s1 : "?", sizeof(E.dm_from));
            strlcpy(E.dm_text, e.s2, sizeof(E.dm_text));
            E.pigeon_on = true;
            E.pigeon_t = s_t;
            E.env_on = E.env_fetch = false;
            break;
        }
    }
    // dados do dashd: avião e barco novos (copiados sob o trinco, formatados fora dele)
    bool np = false, nb = false;
    char pid[16], bname[24], lbl[64];
    int palt = 0, bdist = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_data.fresh_plane) { np = true; s_data.fresh_plane = false; memcpy(pid, s_data.plane, sizeof(pid)); palt = s_data.palt; }
    if (s_data.fresh_boat) { nb = true; s_data.fresh_boat = false; memcpy(bname, s_data.boat, sizeof(bname)); bdist = s_data.bdist; }
    taskEXIT_CRITICAL(&s_mux);
    if (np) snprintf(E.plane_lbl, sizeof(E.plane_lbl), "%.15s %d M", pid, palt);
    if (nb) snprintf(lbl, sizeof(lbl), "%.23s %d,%d MI", bname, bdist / 10, bdist % 10);
    if (np && !E.plane_on) {
        E.plane_on = true;
        E.plane_t = s_t;
        if (!asleep() && R.npath == 0 && R.spot != S_RAIL) act(P_UP, 6);
    }
    if (nb && !E.boat_on) {
        E.boat_on = true;
        E.boat_t = s_t;
        strlcpy(E.boat_lbl, lbl, sizeof(E.boat_lbl));
        if (!asleep() && !night) { go_to(S_RAIL, 0); R.hold_until = s_t + 16; }
    }
}

// microfone: palmas (dois picos curtos) e barulho alto (mais de 1,2 s acima do fundo)
static void mic_step(float dt)
{
    float db = audio_mic_db();
    if (db <= -119) return;
    if (E.mic_base == 0) E.mic_base = db;
    float over = db - E.mic_base;
    if (over < 6) E.mic_base += (db - E.mic_base) * fminf(1, dt * 0.3f);
    if (over > 18) {
        if (E.loud_since == 0) E.loud_since = s_t;
        if (s_t - E.loud_since > 1.2f) { varanda_event(VEV_NOISE, 0, 0, NULL, NULL); E.loud_since = s_t + 6; }
    } else {
        if (E.loud_since > 0 && E.loud_since <= s_t && s_t - E.loud_since < 0.35f) {   // pico curto
            if (s_t - E.clap_t < 0.8f) { varanda_event(VEV_CLAP, 0, 0, NULL, NULL); E.clap_t = 0; }
            else E.clap_t = s_t;
        }
        if (E.loud_since <= s_t) E.loud_since = 0;
    }
}

static void step(float dt, int night)
{
    handle_events(night);
    mic_step(dt);
    // a noite manda o Romeu para a cama; de dia acorda sozinho
    if (night && !asleep() && R.npath == 0 && R.target != S_BED && s_t > R.hold_until) go_to(S_BED, 0);
    if (!night && asleep() && s_t > R.idle_next) go_to(S_DECK, 0);
    // o envelope: o Romeu vai buscá-lo sozinho
    if (E.env_on && !E.env_fetch && s_t - E.env_fetch_t > 2.5f && !asleep() && R.npath == 0) {
        E.env_fetch = true;
        go_to(S_ENV, 0);
        R.hold_until = s_t + 10;
    }
    // caminho
    if (R.npath) {
        pt_t p = R.path[0];
        float dx = p.x - R.x, dy = p.y - R.y, d = sqrtf(dx * dx + dy * dy), sp = R.speed * dt;
        if (R.hop[0] && R.hop_t < 0) R.hop_t = 0;
        if (d <= sp) {
            R.x = p.x;
            R.y = p.y;
            memmove(R.path, R.path + 1, sizeof(R.path[0]) * 3);
            memmove(R.hop, R.hop + 1, 3);
            R.npath--;
            R.hop_t = -1;
            if (!R.npath) {
                R.spot = R.target;
                R.pose = SPOT[R.spot].pose;
                R.x = SPOT[R.spot].x;
                R.y = SPOT[R.spot].y;
                if (R.spot == S_BED && !night) R.pose = P_LIE;
                if (R.spot == S_ENV && E.env_on) {
                    E.env_on = false;
                    E.bar_until = 0;
                    R.carry = true;
                    act(P_ENV, 3);
                    E.msg_until = s_t + 9;
                }
                if (R.spot == S_BOWL) R.hold_until = s_t + 4;
            }
        } else {
            R.x += dx / d * sp;
            R.y += dy / d * sp;
            if (fabsf(dx) > 0.5f) R.dir = dx > 0 ? 1 : -1;
            if (R.hop_t >= 0) R.hop_t += dt;
        }
    }
    if (R.act >= 0 && s_t > R.act_until) R.act = -1;
    // inclinar o cubo: escorrega devagar no deck
    imu_info_t im;
    imu_get(&im);
    if (im.ok && R.npath == 0 && (R.spot == S_DECK || R.spot == S_LIE) && fabsf(im.ay) > 3.0f) {
        R.x += (im.ay > 0 ? -1 : 1) * (fabsf(im.ay) - 3.0f) * 6 * dt;
        R.x = fmaxf(160, fminf(330, R.x));
    }
    // passeios quando não há nada a acontecer
    if (!night && R.npath == 0 && R.act < 0 && s_t > R.hold_until && s_t > R.idle_next) {
        static const uint8_t OPTS[] = {S_DECK, S_RAIL, S_LIE, S_DECK, S_BED, S_RAIL, S_DECK};
        int s = OPTS[rand() % sizeof(OPTS)];
        if (s != R.spot) go_to(s, 0);
        else if (rand() % 2) act(P_FRONT, 3);
        else act(rand() % 3 ? P_WAG : P_YAWN, 2);
        R.idle_next = s_t + 9 + (rand() % 7);
        if (s == S_BED) R.idle_next = s_t + 25;
    }
}

// ------------------------------------------------------------------------------------------ desenho
static uint16_t *B;
static int s_mode;

static inline uint16_t tint565(uint16_t c)
{
    if (s_mode == M_DAY) return c;
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    mode_rgb(s_mode, &r, &g, &b);
    return rgb565(r, g, b);
}

static inline void pset(int x, int y, uint16_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) B[y * W + x] = c;
}

// só no céu e no mar (por trás do parapeito, dos postes e da ilha)
static inline void oset(int x, int y, uint16_t c)
{
    if ((unsigned)x < W && (unsigned)y < H && s_out[y * W + x]) B[y * W + x] = c;
}

static void orect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) oset(i, j, c);
}

static void prect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) pset(i, j, c);
}

static void sprite(int k, int x, int y, bool flip)
{
    if (k < 0) return;
    const spr_t *s = &s_spr[k];
    for (int j = 0; j < s->h; j++) {
        int yy = y - s->ay + j;
        if ((unsigned)yy >= H) continue;
        for (int i = 0; i < s->w; i++) {
            if (!s->a[j * s->w + i]) continue;
            int xx = flip ? x + s->ax - 1 - i : x - s->ax + i;
            if ((unsigned)xx < W) B[yy * W + xx] = tint565(s->px[j * s->w + i]);
        }
    }
}

static void shadow(int cx, int cy, int rx, int ry)
{
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++) {
            if (x * x * ry * ry + y * y * rx * rx > rx * rx * ry * ry) continue;
            int xx = cx + x, yy = cy + y;
            if ((unsigned)xx >= W || (unsigned)yy >= H) continue;
            uint16_t c = B[yy * W + xx];
            B[yy * W + xx] = ((c >> 1) & 0x7BEF) + ((c >> 2) & 0x39E7);   // ~75%
        }
}

static void cloud(int x, int y, float s, uint16_t top, uint16_t bot)
{
    static const int8_t BL[5][3] = {{0, 0, 26}, {-26, 8, 18}, {26, 6, 20}, {10, -12, 18}, {-10, -6, 16}};
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < 5; k++) {
            int rr = (int)(BL[k][2] * s);
            for (int j = -rr; j <= rr; j += 4)
                for (int i = -rr; i <= rr; i += 4) {
                    if (i * i + j * j > rr * rr) continue;
                    bool low = j > rr / 4;
                    if (pass == 0 && low) orect(((x + (int)(BL[k][0] * s) + i) & ~3), ((y + BL[k][1] + j) & ~3), 4, 4, bot);
                    if (pass == 1 && !low) orect(((x + (int)(BL[k][0] * s) + i) & ~3), ((y + BL[k][1] + j) & ~3), 4, 4, top);
                }
        }
}

static uint32_t s_rs = 1;
static inline int rnd(int n)
{
    s_rs = s_rs * 1664525u + 1013904223u;
    return (int)((s_rs >> 8) % (uint32_t)n);
}

static void draw_outside(float t, int night)
{
    if (night) {
        uint32_t keep = s_rs;
        s_rs = 11;
        for (int i = 0; i < 60; i++) {
            int x = rnd(W), y = rnd(140);
            int a = (int)(160 + 90 * sinf(t * 1.3f + i));
            oset(x, y, rgb565(a, a, a + 10 > 255 ? 255 : a + 10));
            oset(x + 1, y, rgb565(a / 2, a / 2, a / 2));
        }
        s_rs = keep;
        for (int y = -14; y <= 14; y++)
            for (int x = -14; x <= 14; x++) {
                if (x * x + y * y > 196) continue;
                bool shadowed = (x - 6) * (x - 6) + (y + 5) * (y + 5) <= 144;
                if (!shadowed) oset(380 + x, 58 + y, rgb565(237, 233, 210));
            }
    } else {
        uint16_t top = s_mode == M_STORM ? rgb565(142, 150, 168) : s_mode == M_DUSK ? rgb565(255, 224, 204) : rgb565(255, 255, 255);
        uint16_t bot = s_mode == M_STORM ? rgb565(94, 102, 122) : s_mode == M_DUSK ? rgb565(224, 160, 144) : rgb565(207, 224, 242);
        static const int16_t CL[7][3] = {{60, 40, 10}, {250, 70, 8}, {400, 30, 12}, {160, 110, 6}, {110, 60, 13}, {330, 90, 11}, {20, 120, 9}};
        int n = s_mode == M_STORM ? 7 : 4;
        for (int i = 0; i < n; i++) {
            int x = (int)fmodf(CL[i][0] + t * (4 + i), 620) - 70;
            cloud(x, CL[i][1], CL[i][2] / 10.0f, top, bot);
        }
    }
    // avião (ADS-B)
    if (E.plane_on) {
        float k = (t - E.plane_t) / 11;
        if (k > 1) E.plane_on = false;
        else {
            int x = (int)(-30 + k * 540), y = (int)(96 - k * 20);
            E.px = x;
            E.py = y;
            if (night) {
                if ((int)(t * 2) % 2) orect(x, y, 4, 4, rgb565(255, 84, 112));
                orect(x + 14, y, 2, 2, 0xFFFF);
            } else {
                orect(x, y + 4, 24, 4, rgb565(244, 247, 251));
                orect(x + 8, y - 8, 6, 20, rgb565(230, 237, 243));
                orect(x, y, 2, 8, rgb565(230, 237, 243));
                orect(x + 4, y + 12, 20, 2, rgb565(184, 196, 216));
                orect(x + 44, y + 4, 2, 2, rgb565(255, 84, 112));
            }
        }
    }
    // barco (AIS)
    if (E.boat_on) {
        float k = (t - E.boat_t) / 28;
        if (k > 1) E.boat_on = false;
        else {
            int x = (int)(440 - k * 520), y = 196;
            E.bx = x;
            if (night) {
                orect(x + 34, y - 8, 4, 4, rgb565(255, 226, 122));
                orect(x + 4, y - 4, 2, 2, (int)(t * 2) % 2 ? rgb565(255, 84, 112) : rgb565(90, 32, 48));
            } else {
                orect(x, y, 48, 6, rgb565(122, 40, 40));
                orect(x + 4, y + 6, 40, 2, rgb565(74, 24, 24));
                orect(x + 34, y - 10, 12, 10, rgb565(232, 236, 242));
                orect(x + 4, y - 6, 8, 6, rgb565(224, 160, 48));
                orect(x + 14, y - 6, 8, 6, rgb565(74, 144, 208));
                orect(x + 24, y - 4, 6, 4, rgb565(95, 176, 96));
                orect(x + 48, y + 6, 12, 2, rgb565(220, 230, 240));
            }
        }
    }
    // chuva e relâmpagos (ou neve)
    if (E.wx >= 1) {
        uint32_t keep = s_rs;
        s_rs = 3;
        for (int i = 0; i < 120; i++) {
            int x0 = rnd(W), y0 = rnd(340);
            if (E.wx == 3) {
                int x = (int)fmodf(x0 + 10 * sinf(t + i), W), y = (int)fmodf(y0 + t * 40, 345);
                orect(x, y, 3, 3, rgb565(240, 244, 255));
            } else {
                int x = (int)fmodf(x0 + t * 60, 500) - 10, y = (int)fmodf(y0 + t * 280, 350);
                orect(x, y, 2, 6, rgb565(170, 195, 235));
            }
        }
        s_rs = keep;
        if (E.wx == 2) {
            if (rnd(250) == 0) E.light_t = t;
            if (t - E.light_t < 0.12f)
                for (int i = 0; i < W * 345; i++)
                    if (s_out[i]) B[i] = ((B[i] >> 1) & 0x7BEF) + 0x7BEF;
        }
    }
}

static const char *const HEART[5] = {".K.K.", "KPKPK", "KPPPK", ".KPK.", "..K.."};
static const char *const PIGEON[2][5] = {{"..KK....", ".KGGK...", "KDGGGKK.", ".KGGGGGK", "..KKKKK."},
                                          {"..KK.KK.", ".KGGKGGK", "KDGGGKK.", ".KGGGGGK", "..KKKKK."}};
static void pix(const char *const *rows, int nr, int x, int y, int s)
{
    for (int j = 0; j < nr; j++)
        for (int i = 0; rows[j][i]; i++) {
            char k = rows[j][i];
            if (k == '.') continue;
            uint16_t c = k == 'K' ? rgb565(46, 42, 54) : k == 'G' ? rgb565(184, 192, 208) : k == 'D' ? rgb565(136, 144, 160) : rgb565(232, 122, 154);
            prect(x + i * s, y + j * s, s, s, tint565(c));
        }
}

static void text_box(int x, int y, int w, int h, uint16_t bg, uint16_t edge)
{
    prect(x, y, w, h, bg);
    prect(x, y, w, 2, edge);
    prect(x, y + h - 2, w, 2, edge);
    prect(x, y, 2, h, edge);
    prect(x + w - 2, y, 2, h, edge);
}

static void label(const char *s, int x, int y, int sc, uint32_t col)
{
    pfont_draw((uint32_t *)B, W, H, W, s, x, y, sc, col, 0x101010, -1);
}

static void draw_world(float t, int night)
{
    // tigela ao lado da caixa (a pose a beber já a traz)
    if (!(R.npath == 0 && R.spot == S_BOWL)) sprite(s_bowl, SPOT[S_BOWL].x, SPOT[S_BOWL].y, false);
    // pombo que traz a DM e pousa no poste
    if (E.pigeon_on) {
        float k = fminf(1, (t - E.pigeon_t) / 2);
        int x = (int)(500 - k * 376), y = (int)(60 + k * 132), f = k < 1 ? (int)(t * 10) % 2 : 0;
        pix(PIGEON[f], 5, x, y, 2);
        if (k >= 1 && !E.env_on && !E.env_fetch) {
            E.env_on = true;
            E.env_fetch = false;
            E.env_fetch_t = t;
            E.bar_until = t + 60;
        }
        if (t - E.pigeon_t > 7) E.pigeon_on = false;
    }
    if (E.env_on) {
        int x = 120, y = 374;
        prect(x - 2, y - 2, 24, 18, tint565(rgb565(14, 59, 28)));
        prect(x, y, 20, 14, tint565(rgb565(46, 160, 67)));
        for (int i = 0; i < 5; i++) {
            prect(x + i * 2, y + (i >> 1) * 2, 2, 2, tint565(rgb565(103, 234, 148)));
            prect(x + 18 - i * 2, y + (i >> 1) * 2, 2, 2, tint565(rgb565(103, 234, 148)));
        }
    }
    (void)night;
}

static int pose_frame(int pose, float fps)
{
    int n = s_nfr[pose];
    if (n <= 0) return -1;
    return s_pose[pose][(int)(s_t * fps) % n];
}

static void draw_romeu(float t, int night)
{
    int pose = R.act >= 0 ? R.act : R.pose;
    bool walking = R.npath > 0;
    float hop = R.hop_t >= 0 ? sinf(fminf(1, R.hop_t / 0.5f) * 3.14159f) * 18 : 0;
    int x = (int)R.x, y = (int)R.y;
    shadow(x, y - 1, pose == P_SLEEP || pose == P_LIE ? 28 : 20, 4);
    int k;
    bool flip = false;
    if (walking) {
        k = pose_frame(hop > 1 && s_nfr[P_JUMP] ? P_JUMP : P_WALK, 8);
        flip = R.dir < 0;
        y -= (int)hop;
    } else {
        // a dormir, um quadro só: os dois quadros "a respirar" não têm o cão no mesmo sítio e ele parecia mexer-se
        float fps = pose == P_SLEEP ? 0 : pose == P_WAG ? 6 : pose == P_SPIN ? 5 : pose == P_SHAKE ? 8 : pose == P_BARK ? 5 : pose == P_PANT ? 3 : pose == P_FRONT ? 0.5f : 4;
        k = pose_frame(pose, fps);
        if (k < 0) k = pose_frame(P_SIT, 1);
        if (R.carry && pose != P_ENV) R.carry = false;
    }
    sprite(k, x, y, flip);
    if (R.npath == 0 && R.spot == S_HIDE) {   // a caixa tapa-o: volta a desenhar o fundo dela por cima
        for (int yy = 296; yy < 388; yy++)
            for (int xx = 328; xx < 420; xx++) B[yy * W + xx] = s_lut[s_mode][s_bg[yy * W + xx]];
    }
    if (t < R.hearts_until) {
        float f = fmodf(R.hearts_until - t, 1);
        pix(HEART, 5, x + 14, (int)(y - 92 - (1 - f) * 12), 3);
    }
    if (pose == P_SLEEP && !walking) {   // zzz
        float z = fmodf(t * 0.7f, 1);
        label("Z", x + 28 + (int)(z * 6), (int)(y - 44 - z * 14), 2, 0xDCE2FA);
        label("Z", x + 40 + (int)(z * 6), (int)(y - 60 - z * 14), 1, 0xDCE2FA);
    }
    (void)night;
}

static void draw_overlay(float t, int night)
{
    // hora no canto (pequena, pixel)
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) {   // longe dos cantos arredondados do ecrã, numa faixa escura translúcida
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
        int tw = pfont_width(hm, 3), x0 = 34, y0 = 26;
        for (int y = y0 - 8; y < y0 + 21 + 8; y++)
            for (int x = x0 - 12; x < x0 + tw + 12; x++) {
                uint16_t c = B[y * W + x];
                B[y * W + x] = (c >> 2) & 0x39E7;   // 25% do fundo
            }
        label(hm, x0, y0, 3, 0xFFFFFF);
    }
    if (E.plane_on && !night && R.act == P_UP) label(E.plane_lbl, (int)fmaxf(8, fminf(380, E.px - 20)), (int)E.py + 24, 1, 0xFFFFFF);
    if (E.boat_on && R.spot == S_RAIL && R.npath == 0 && E.bx > -40 && E.bx < 440)
        label(E.boat_lbl, (int)fmaxf(8, fminf(340, E.bx - 10)), 214, 1, 0xFFFFFF);
    if (E.msg_until > t) {
        text_box(30, 150, 420, 110, tint565(rgb565(14, 26, 18)), rgb565(46, 160, 67));
        char hd[40];
        snprintf(hd, sizeof(hd), "DM DE %s", E.dm_from);
        label(hd, 46, 166, 2, 0x67EA94);
        // texto em duas linhas de até 26 carateres
        char l1[30] = "", l2[30] = "";
        const char *s = E.dm_text;
        int n = strlen(s);
        if (n <= 26) strlcpy(l1, s, sizeof(l1));
        else {
            int cut = 26;
            while (cut > 10 && s[cut] != ' ') cut--;
            memcpy(l1, s, cut);
            l1[cut] = 0;
            strlcpy(l2, s + cut + (s[cut] == ' '), 27);
        }
        label(l1, 46, 196, 2, 0xE7FFE9);
        label(l2, 46, 220, 2, 0xE7FFE9);
    } else if (E.env_on && E.bar_until > t) {
        prect(0, 446, W, 34, rgb565(10, 8, 6));
        char b[48];
        snprintf(b, sizeof(b), "DM DE %s", E.dm_from);
        label(b, 14, 456, 2, 0x67EA94);
    }
}

void varanda_draw(uint16_t *buf, float t)
{
    if (!s_loaded) return;
    float dt = t - s_t;
    if (dt < 0 || dt > 0.25f) dt = 0.05f;
    s_t = t;
    int night;
    s_mode = cur_mode(&night);
    step(dt, night);
    B = buf;
    const uint16_t *lut = s_lut[s_mode];
    for (int i = 0; i < W * H; i++) B[i] = lut[s_bg[i]];
    draw_outside(t, night);
    draw_world(t, night);
    draw_romeu(t, night);
    draw_overlay(t, night);
}
