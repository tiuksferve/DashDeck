// Interface do Deck³. Estrutura do ecrã (480×480, AMOLED: fundo preto puro = píxeis apagados):
//   cabeçalho (56 px): ← · título · pontos de página · hora · Wi-Fi · bateria
//   corpo: tela do host (menu em boxes com voxels, list, kv, text, botões) ou o ecrã de arranque
//   camada de cima: avisos (toast), confirmações, notificações em voxels, progresso, painel de acordar
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "anim.h"
#include "audio.h"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "deck.h"
#include "deck_fonts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "imu.h"
#include "link.h"
#include "mbedtls/base64.h"
#include "net.h"
#include "plot.h"
#include "power.h"
#include "sd.h"
#include "store.h"
#include "voxel.h"

static const char *TAG = "ui";

#define W        480
#define H        480
#define HDR_H    56
#define C_BG     0x000000
#define C_CARD   0x10151C
#define C_LINE   0x1E2630
#define C_TEXT   0xE8F0FF
#define C_SUB    0x8A97A8
#define C_ACCENT 0x3EC6FF
#define C_OK     0x4BE38A
#define C_WARN   0xFFB23E
#define C_ERR    0xFF5470
#define MAX_REFS 160
#define MAX_BUFS 72

// ------------------------------------------------------------------------------------------ estado

static lv_obj_t *s_scr, *s_hdr, *s_back, *s_title, *s_dots, *s_clock, *s_wifi, *s_bat, *s_link, *s_body;
static lv_obj_t *s_tbox, *s_tsub;   // título: "Config · Ecrã e som" = s_title (28) + s_tsub (20, cinzento)
static char s_id[48];             // id da tela do host aberta ("" = arranque/offline)
static cJSON *s_screen;           // cópia da última "screen" (as "update" juntam-se a ela)
static char *s_refs[MAX_REFS];    // ids dos itens/botões (user_data dos eventos)
static int s_nrefs;
static void *s_bufs[MAX_BUFS];    // buffers dos canvas do corpo
static int s_nbufs;
static char s_status[64];         // texto do host para o cabeçalho (ex.: bateria do uConsole)

// arranque / offline: ecrã inteiro por baixo do cabeçalho, desenhado em anim.c
#define BOOT_W W
#define BOOT_H (H - HDR_H)
static lv_obj_t *s_boot_cv;
static uint32_t *s_boot_buf;
static lv_timer_t *s_boot_tm;
static int64_t s_boot_t0;
static bool s_boot_first = true;   // o 1.º arranque mostra a sequência toda (como no DashDeck)
static bool s_boot_min;            // este ecrã de arranque tem de passar a sequência toda
static bool s_boot_exit;           // chegou a 1.ª tela: fecha a barra e sai
static float s_boot_prog, s_boot_exit_t;
static bool s_away;                // modo "fora de casa": relógio em vez da animação (sem o uConsole)
static int64_t s_last_flush;

// imagem de fundo da tela ("bg") e cena em voxels (tpl "vox")
static lv_obj_t *s_bgimg;
static lv_obj_t *s_scene_cv;
static uint32_t *s_scene_buf;
static const vox_model_t *s_scene_m;
static lv_timer_t *s_scene_tm;
static int64_t s_scene_t0;
#define SCENE_W 444
#define SCENE_H 330

// entrada das telas: boxes a subir em cascata, ícones a dar uma volta, página a deslizar
typedef struct {
    lv_obj_t *cv;
    uint32_t *buf;
    const vox_model_t *m;
    uint32_t tint;
    int size;
} icon_t;
#define MAX_ICONS 24
static icon_t s_icons[MAX_ICONS];
static int s_nicons;
static lv_timer_t *s_intro_tm;
static int64_t s_intro_t0;
static int s_page = -1;

// "Sobre o Deck³" (toque no título da tela inicial)
static lv_obj_t *s_about, *s_about_cv, *s_about_txt;
static uint32_t *s_about_buf;
static lv_timer_t *s_about_tm;
static int64_t s_about_t0;
static cJSON *s_about_lines;       // linhas de créditos enviadas pelo deckd ({"t":"about"})

// ecrã apagado
static int s_dim_s = 60, s_off_s = 300, s_bl = 80;
static int s_bl_state;            // 0 normal, 1 escurecido, 2 apagado
static lv_obj_t *s_wake_pane;

// notificação ("Ondas LoRa" em voxels)
#define NCV_W 360
#define NCV_H 320
static lv_obj_t *s_notif, *s_notif_cv, *s_notif_band;
static uint32_t *s_notif_buf;
static lv_timer_t *s_notif_tm;
static int64_t s_notif_t0;
static float s_notif_total;

static lv_obj_t *s_confirm, *s_prog, *s_prog_bar, *s_prog_lbl;
static int64_t s_offline_since;
static bool s_dragging, s_rebuild_pending;   // slider a ser arrastado: adia os redesenhos
static int s_bat_warned = 101;
static volatile bool s_bl_dirty;                // último nível de aviso de bateria fraca

// ------------------------------------------------------------------------------------------ util

static const char *jstr(const cJSON *o, const char *k)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
}

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static uint32_t color_of(const char *s, uint32_t def)
{
    if (!s || !*s) return def;
    if (*s == '#') return (uint32_t)strtoul(s + 1, NULL, 16) & 0xFFFFFF;
    if (!strcmp(s, "ok")) return C_OK;
    if (!strcmp(s, "warn")) return C_WARN;
    if (!strcmp(s, "err")) return C_ERR;
    if (!strcmp(s, "sub")) return C_SUB;
    if (!strcmp(s, "accent")) return C_ACCENT;
    return def;
}

static char *ref_add(char kind, const char *id)
{
    if (s_nrefs >= MAX_REFS || !id) return NULL;
    size_t n = strlen(id);
    char *r = malloc(n + 2);
    r[0] = kind;
    memcpy(r + 1, id, n + 1);
    s_refs[s_nrefs++] = r;
    return r;
}

// memória pequena que vive com a tela (libertada em body_free, como os refs)
static void *ref_mem(size_t bytes)
{
    if (s_nrefs >= MAX_REFS) return NULL;
    void *m = calloc(1, bytes);
    if (m) s_refs[s_nrefs++] = m;
    return m;
}

static void *buf_add(size_t bytes)
{
    if (s_nbufs >= MAX_BUFS) return NULL;
    void *b = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    if (b) s_bufs[s_nbufs++] = b;
    return b;
}

static void body_free(void)
{
    if (s_scene_tm) lv_timer_delete(s_scene_tm);
    s_scene_tm = NULL;
    s_scene_cv = NULL;
    if (s_intro_tm) lv_timer_delete(s_intro_tm);
    s_intro_tm = NULL;
    s_nicons = 0;
    for (int i = 0; i < s_nrefs; i++) free(s_refs[i]);
    s_nrefs = 0;
    lv_obj_clean(s_body);   // os canvas saem antes dos buffers
    for (int i = 0; i < s_nbufs; i++) heap_caps_free(s_bufs[i]);
    s_nbufs = 0;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);   // sem os estilos do tema do LVGL (sombras, filtros de cor ao tocar)
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static int canvas_stride(lv_obj_t *cv)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);
    return db ? db->header.stride / 4 : 0;
}

// Canvas ARGB8888 com um modelo de voxels (ícones das boxes, estáticos).
static lv_obj_t *vox_icon(lv_obj_t *parent, const char *model, uint32_t tint, int size)
{
    const vox_model_t *m = vox_get(model);
    if (!m) m = vox_get("block");
    uint32_t *buf = buf_add(size * size * 4);
    lv_obj_t *cv = lv_canvas_create(parent);
    if (!buf) return cv;
    lv_canvas_set_buffer(cv, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    vox_view_t v = VOX_VIEW_DEFAULT;
    v.tint = tint;
    vox_render(m, &v, buf, size, size, canvas_stride(cv));
    lv_obj_invalidate(cv);
    if (s_nicons < MAX_ICONS) s_icons[s_nicons++] = (icon_t){.cv = cv, .buf = buf, .m = m, .tint = tint, .size = size};
    return cv;
}

// Ícones das boxes enviados pelo dashd (deck3/icons/<id>.bin, imagem do LVGL ARGB8888): lidos do cartão uma
// vez e guardados na PSRAM. O menu é refeito a cada "update"; ler 20 ficheiros de cada vez segurava o LVGL
// segundos (e as mensagens seguintes eram descartadas).
#define ICON_CACHE 64
typedef struct {
    char rel[48];
    lv_image_dsc_t dsc;
} icon_cache_t;
static icon_cache_t s_icache[ICON_CACHE];
static int s_nicache;

static const lv_image_dsc_t *icon_cached(const char *rel)
{
    for (int i = 0; i < s_nicache; i++)
        if (!strcmp(s_icache[i].rel, rel)) return &s_icache[i].dsc;
    if (s_nicache >= ICON_CACHE || strlen(rel) >= sizeof(s_icache[0].rel)) return NULL;
    char p[128];
    if (!sd_path(rel, p, sizeof(p))) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    lv_image_header_t h;
    uint8_t *data = NULL;
    size_t n = 0;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == LV_IMAGE_HEADER_MAGIC && h.w && h.h && h.w <= 256 &&
        h.h <= 256) {
        n = (size_t)h.stride * h.h;
        data = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (data && fread(data, 1, n, f) != n) {
            heap_caps_free(data);
            data = NULL;
        }
    }
    fclose(f);
    if (!data) return NULL;
    icon_cache_t *c = &s_icache[s_nicache++];
    strlcpy(c->rel, rel, sizeof(c->rel));
    c->dsc = (lv_image_dsc_t){.header = h, .data_size = n, .data = data};
    return &c->dsc;
}

// ------------------------------------------------------------------------------------------ envio

static void send_tap(const char *kind, const char *key, const char *val)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", kind);
    cJSON_AddStringToObject(o, "id", s_id);
    if (key) cJSON_AddStringToObject(o, key, val);
    link_send(o);
    cJSON_Delete(o);
}

static void fire_ref(const char *r)
{
    if (!r) return;
    if (r[0] == 'i') send_tap("tap", "item", r + 1);
    else if (r[0] == 'b') send_tap("btn", "btn", r + 1);
    else if (r[0] == 'd') link_send_event("dock", "item", r + 1, NULL, NULL);
}

// --------------------------------------------------------------------------------------- confirmar

static char *s_confirm_ref;

static void confirm_close(void)
{
    if (s_confirm) {
        lv_obj_delete(s_confirm);
        s_confirm = NULL;
    }
    free(s_confirm_ref);
    s_confirm_ref = NULL;
}

static void confirm_cb(lv_event_t *e)
{
    bool yes = (bool)(uintptr_t)lv_event_get_user_data(e);
    char *r = s_confirm_ref;
    s_confirm_ref = NULL;
    confirm_close();
    if (yes) fire_ref(r);
    free(r);
}

static void confirm_open(const char *text, const char *ref)
{
    confirm_close();
    s_confirm_ref = strdup(ref);
    s_confirm = box(lv_layer_top(), 0x000000, 0);
    lv_obj_set_size(s_confirm, W, H);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_80, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *card = box(s_confirm, C_CARD, 24);
    lv_obj_set_size(card, 400, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 20, 0);
    lv_obj_t *l = label(card, text, &deck_font_20, C_TEXT);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_t *row = box(card, C_CARD, 0);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 16, 0);
    const char *txt[2] = {"Não", "Sim"};
    uint32_t col[2] = {0x2A323D, C_ACCENT};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = box(row, col[i], 18);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 64);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, confirm_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_center(label(b, txt[i], &deck_font_20, i ? 0x001018 : C_TEXT));
    }
}

static void ref_clicked(lv_event_t *e)
{
    const char *r = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    const char *conf = lv_obj_get_user_data(t);   // texto de confirmação (opcional)
    if (conf) confirm_open(conf, r);
    else fire_ref(r);
}

static void make_clickable(lv_obj_t *o, char kind, const char *id, const char *confirm)
{
    char *r = ref_add(kind, id);
    if (!r) return;
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1A222D), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(o, 245, LV_STATE_PRESSED);
    char *c = confirm ? ref_add('c', confirm) : NULL;
    if (c) lv_obj_set_user_data(o, c + 1);
    lv_obj_add_event_cb(o, ref_clicked, LV_EVENT_CLICKED, r);
}

// ------------------------------------------------------------------------------------------ toast

static void anim_ty_cb(void *o, int32_t v)
{
    lv_obj_set_style_translate_y(o, v, 0);
}

static void anim_tx_cb(void *o, int32_t v)
{
    lv_obj_set_style_translate_x(o, v, 0);
}

static void toast_del(lv_timer_t *t)
{
    lv_obj_delete(lv_timer_get_user_data(t));
}

static void toast_nolock(const char *text, const char *level)
{
    uint32_t c = color_of(level, C_ACCENT);
    if (level && !strcmp(level, "info")) c = C_ACCENT;
    lv_obj_t *t = box(lv_layer_top(), 0x151B23, 20);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(c), 0);
    lv_obj_set_style_pad_hor(t, 22, 0);
    lv_obj_set_style_pad_ver(t, 14, 0);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_t *l = label(t, text, &deck_font_20, C_TEXT);
    lv_obj_set_style_max_width(l, 380, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_anim_t a;   // sobe de baixo
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, anim_ty_cb);
    lv_anim_set_values(&a, 90, 0);
    lv_anim_set_duration(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_timer_t *tm = lv_timer_create(toast_del, 2500, t);
    lv_timer_set_repeat_count(tm, 1);
}

void ui_toast(const char *text, const char *level)
{
    if (bsp_display_lock(1000)) {
        toast_nolock(text, level);
        bsp_display_unlock();
    }
}

void ui_progress(const char *text, int pct)
{
    if (!bsp_display_lock(1000)) return;
    if (pct < 0) {
        if (s_prog) lv_obj_delete(s_prog);
        s_prog = NULL;
    } else {
        if (!s_prog) {
            s_prog = box(lv_layer_top(), 0x000000, 0);
            lv_obj_set_size(s_prog, W, H);
            lv_obj_set_style_bg_opa(s_prog, LV_OPA_80, 0);
            s_prog_lbl = label(s_prog, "", &deck_font_28, C_TEXT);
            lv_obj_align(s_prog_lbl, LV_ALIGN_CENTER, 0, -40);
            s_prog_bar = lv_bar_create(s_prog);
            lv_obj_set_size(s_prog_bar, 320, 18);
            lv_obj_align(s_prog_bar, LV_ALIGN_CENTER, 0, 20);
            lv_obj_set_style_bg_color(s_prog_bar, lv_color_hex(C_LINE), 0);
            lv_obj_set_style_bg_color(s_prog_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        }
        lv_label_set_text_fmt(s_prog_lbl, "%s %d%%", text ? text : "", pct);
        lv_bar_set_value(s_prog_bar, pct, LV_ANIM_OFF);
    }
    bsp_display_unlock();
}

// ------------------------------------------------------------------------------------ brilho / idle

static void wake_nolock(void)
{
    if (s_bl_state) {
        bsp_display_brightness_set(s_bl);
        s_bl_state = 0;
    }
    if (s_wake_pane) {
        lv_obj_delete(s_wake_pane);
        s_wake_pane = NULL;
    }
    lv_display_trigger_activity(NULL);
}

void ui_wake(void)
{
    if (bsp_display_lock(500)) {
        wake_nolock();
        bsp_display_unlock();
    }
}

static void wake_pane_cb(lv_event_t *e)
{
    wake_nolock();   // este toque só acorda, não abre nada
}

static void idle_check(void)
{
    if (s_notif || s_prog) return;
    uint32_t idle = lv_display_get_inactive_time(NULL) / 1000;
    if (s_bl_state == 0 && s_dim_s > 0 && idle >= (uint32_t)s_dim_s) {
        bsp_display_brightness_set(s_bl > 10 ? 10 : s_bl);
        s_bl_state = 1;
        if (!s_wake_pane) {
            s_wake_pane = box(lv_layer_top(), 0, 0);
            lv_obj_set_size(s_wake_pane, W, H);
            lv_obj_set_style_bg_opa(s_wake_pane, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(s_wake_pane, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_wake_pane, wake_pane_cb, LV_EVENT_PRESSED, NULL);
        }
    } else if (s_bl_state == 1 && s_off_s > 0 && idle >= (uint32_t)s_off_s) {
        bsp_display_brightness_set(0);
        s_bl_state = 2;
    }
}

// ------------------------------------------------------------------------------------------ cabeçalho

static void back_cb(lv_event_t *e)
{
    if (s_id[0]) send_tap("back", NULL, NULL);
}

static void hdr_refresh(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > 120) lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else lv_label_set_text(s_clock, "--:--");
    lv_obj_update_layout(s_clock);
    lv_obj_align_to(s_link, s_clock, LV_ALIGN_OUT_LEFT_MID, -8, 0);   // a hora muda de largura

    net_info_t ni;
    net_get_info(&ni);
    uint32_t wc = ni.rssi == 0 ? 0x3A424D : ni.rssi > -60 ? C_OK : ni.rssi > -72 ? C_WARN : C_ERR;
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(wc), 0);
    bool online = link_host_alive() || net_ws_connected();
    lv_obj_set_style_bg_color(s_link, lv_color_hex(online ? (link_active() == LINK_USB ? C_ACCENT : C_OK) : C_ERR), 0);

    power_info_t p;
    power_get(&p);
    const char *sym = LV_SYMBOL_BATTERY_EMPTY;
    if (p.percent > 85) sym = LV_SYMBOL_BATTERY_FULL;
    else if (p.percent > 60) sym = LV_SYMBOL_BATTERY_3;
    else if (p.percent > 35) sym = LV_SYMBOL_BATTERY_2;
    else if (p.percent > 12) sym = LV_SYMBOL_BATTERY_1;
    if (p.percent < 0) lv_label_set_text(s_bat, p.usb ? LV_SYMBOL_USB : "");
    else lv_label_set_text_fmt(s_bat, "%s%s %d%%", p.charging ? LV_SYMBOL_CHARGE : "", sym, p.percent);
    lv_obj_update_layout(s_bat);
    lv_obj_align_to(s_wifi, s_bat, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_set_style_text_color(s_bat, lv_color_hex(p.percent >= 0 && p.percent <= 15 && !p.charging ? C_ERR : C_SUB), 0);
}

// Páginas: pontos desenhados (a fonte não tem o ●); a página atual é um traço mais largo, na cor de destaque.
static void set_dots(int pages, int page)
{
    lv_obj_clean(s_dots);
    if (pages > 9) pages = 9;
    if (pages < 2) {
        lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    for (int i = 0; i < pages; i++) {
        lv_obj_t *d = box(s_dots, i == page ? C_ACCENT : 0x4A5462, LV_RADIUS_CIRCLE);
        lv_obj_set_size(d, i == page ? 20 : 8, 8);
    }
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
}

// Título do cabeçalho numa só linha: "Config · Ecrã e som" fica "Config" (28) e "Ecrã e som" (20, cinzento)
// a seguir, cortado com reticências se não couber. Um título longo sem " · " desce para a fonte 20.
static int32_t text_w(const char *txt, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

static void set_title(const char *txt)
{
    if (!txt) txt = "";
    char head[96];
    const char *sep = strstr(txt, " \xC2\xB7 ");   // " · "
    size_t hl = sep ? (size_t)(sep - txt) : strlen(txt);
    if (hl >= sizeof(head)) hl = sizeof(head) - 1;
    memcpy(head, txt, hl);
    head[hl] = '\0';
    bool back = !lv_obj_has_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    int32_t avail = W - 44 - 96 - (back ? 52 : 0);   // painel menos os cantos, o estado à direita e o "voltar"
    lv_obj_set_width(s_tbox, avail);
    lv_obj_align(s_tbox, LV_ALIGN_LEFT_MID, back ? 52 : 0, 2);
    const lv_font_t *f = &deck_font_28;
    if (text_w(head, f) > avail) f = &deck_font_20;
    lv_obj_set_style_text_font(s_title, f, 0);
    lv_obj_set_height(s_title, lv_font_get_line_height(f));
    lv_label_set_text(s_title, head);
    int32_t tw = text_w(head, f);
    lv_obj_set_width(s_title, tw > avail ? avail : LV_SIZE_CONTENT);
    if (sep && tw + 40 < avail) {
        lv_label_set_text(s_tsub, sep + 4);   // depois de " · " (o · ocupa 2 bytes)
        lv_obj_remove_flag(s_tsub, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tsub, LV_OBJ_FLAG_HIDDEN);
    }
}

// ------------------------------------------------------------------------------------ ecrã de arranque

static float boot_target(void)
{
    net_info_t ni;
    net_get_info(&ni);
    static const float P[] = {0.05f, 0.2f, 0.35f, 0.55f, 0.7f, 0.85f};
    return P[ni.state <= NET_ONLINE ? ni.state : 0];
}

static void boot_status(char *out, size_t n)
{
    net_info_t ni;
    net_get_info(&ni);
    if (ni.state == NET_ONLINE) snprintf(out, n, "A AGUARDAR O UCONSOLE");
    else if (ni.state == NET_CONNECTING) snprintf(out, n, "A LIGAR A %s", ni.ssid);
    else if (ni.state >= NET_DISCOVERING) snprintf(out, n, "%s  %s", net_state_text(ni.state), ni.ip);
    else snprintf(out, n, "%s", net_state_text(ni.state));
}

static void apply_screen(bool fresh);

static float imu_tilt(void)
{
    imu_info_t im;
    imu_get(&im);
    return im.ok ? fmaxf(-0.25f, fminf(0.25f, im.ay * 0.02f)) : 0;
}

static void boot_stop(void)
{
    if (s_boot_tm) lv_timer_delete(s_boot_tm);
    s_boot_tm = NULL;
    if (s_boot_cv) lv_obj_delete(s_boot_cv);
    s_boot_cv = NULL;
    heap_caps_free(s_boot_buf);
    s_boot_buf = NULL;
}

static void away_draw(float sec)
{
    static const char *DOW[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
    static const char *MON[] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN", "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8] = "--:--", date[24] = "SEM HORA";
    if (tm.tm_year > 120) {
        snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(date, sizeof(date), "%s %d %s", DOW[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
    }
    power_info_t p;
    power_get(&p);
    char st[80];
    net_info_t ni;
    net_get_info(&ni);
    if (ni.state == NET_ONLINE) snprintf(st, sizeof(st), "A AGUARDAR O UCONSOLE");
    else if (ni.state >= NET_DISCOVERING) snprintf(st, sizeof(st), "FORA DE CASA  WI-FI %s", ni.ssid);
    else snprintf(st, sizeof(st), "FORA DE CASA  %s", net_state_text(ni.state));
    away_t a = {.time = hm, .date = date, .bat = p.percent, .chg = p.charging, .pending = sd_events_pending(),
                .sd = sd_ok(), .status = st, .shift = tm.tm_min % 9};
    anim_away_draw(s_boot_buf, BOOT_W, BOOT_H, canvas_stride(s_boot_cv), sec, &a);
    lv_obj_invalidate(s_boot_cv);
}

static void boot_anim(lv_timer_t *t)
{
    if (!s_boot_buf) return;
    float sec = (esp_timer_get_time() - s_boot_t0) / 1e6f;
    if (s_away) {
        if (s_boot_exit) {   // o uConsole voltou
            boot_stop();
            apply_screen(true);
            return;
        }
        away_draw(sec);
        return;
    }
    if (!s_boot_exit && sec > 30) {   // 30 s sem uConsole: passa ao relógio "fora de casa"
        s_away = true;
        lv_timer_set_period(s_boot_tm, 250);
        away_draw(sec);
        return;
    }
    if (s_boot_exit && (!s_boot_min || sec >= BOOT_SEQ)) {
        // a barra fecha e entra a tela inicial
        if (s_boot_exit_t == 0) s_boot_exit_t = sec;
        s_boot_prog += (1.0f - s_boot_prog) * 0.35f;
        if (sec - s_boot_exit_t > 0.4f) {
            boot_stop();
            apply_screen(true);
            return;
        }
    } else {
        s_boot_prog += (boot_target() - s_boot_prog) * 0.06f;
    }
    char st[80] = "";
    if (!s_boot_exit) boot_status(st, sizeof(st));
    anim_boot_draw(s_boot_buf, canvas_stride(s_boot_cv), sec, s_boot_prog, st, imu_tilt());
    lv_obj_invalidate(s_boot_cv);
}

static void boot_show_mode(bool away)
{
    if (s_boot_buf) return;
    body_free();
    s_id[0] = '\0';
    s_page = -1;
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    set_title("");
    set_dots(0, 0);
    s_boot_buf = heap_caps_calloc(1, BOOT_W * BOOT_H * 4, MALLOC_CAP_SPIRAM);
    if (!s_boot_buf) return;
    s_boot_cv = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_boot_cv, s_boot_buf, BOOT_W, BOOT_H, LV_COLOR_FORMAT_XRGB8888);
    lv_obj_set_pos(s_boot_cv, 0, HDR_H);
    s_boot_min = s_boot_first;
    s_boot_first = false;
    s_boot_exit = false;
    s_boot_exit_t = 0;
    s_boot_prog = 0;
    s_away = away;
    if (s_bgimg) lv_obj_add_flag(s_bgimg, LV_OBJ_FLAG_HIDDEN);
    anim_boot_start(BOOT_W, BOOT_H);
    s_boot_t0 = esp_timer_get_time();
    s_boot_tm = lv_timer_create(boot_anim, away ? 250 : 33, NULL);
    boot_anim(NULL);
}

static void boot_show(void)
{
    boot_show_mode(false);
}

// ---- entrada das telas
static void intro_tick(lv_timer_t *t)
{
    float sec = (esp_timer_get_time() - s_intro_t0) / 1e6f;
    bool busy = false;
    for (int i = 0; i < s_nicons; i++) {
        icon_t *ic = &s_icons[i];
        float k = (sec - i * 0.05f) / 0.7f;   // cada ícone dá uma volta e assenta (ease-out)
        if (k < 0) {
            busy = true;
            continue;
        }
        if (k >= 1) k = 1;
        else busy = true;
        float u = 1 - k;
        vox_view_t v = VOX_VIEW_DEFAULT;
        v.tint = ic->tint;
        v.yaw = 0.785f - u * u * u * 3.14159f;
        v.glow = u * 0.8f;
        memset(ic->buf, 0, ic->size * ic->size * 4);
        vox_render(ic->m, &v, ic->buf, ic->size, ic->size, canvas_stride(ic->cv));
        lv_obj_invalidate(ic->cv);
    }
    if (!busy) {
        lv_timer_delete(s_intro_tm);
        s_intro_tm = NULL;
    }
}

static void intro_start(const char *tpl, int slide_dir)
{
    if (slide_dir) {   // mudança de página: o corpo entra do lado do gesto
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_body);
        lv_anim_set_exec_cb(&a, anim_tx_cb);
        lv_anim_set_values(&a, slide_dir * 120, 0);
        lv_anim_set_duration(&a, 260);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    if (tpl && !strcmp(tpl, "menu")) {
        // as boxes sobem em cascata (como cartas a pousar)
        uint32_t n = lv_obj_get_child_count(s_body);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *o = lv_obj_get_child(s_body, i);
            lv_obj_set_style_translate_y(o, 60, 0);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, o);
            lv_anim_set_exec_cb(&a, anim_ty_cb);
            lv_anim_set_values(&a, 60, 0);
            lv_anim_set_delay(&a, i * 50);
            lv_anim_set_duration(&a, 340);
            lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
            lv_anim_start(&a);
        }
    } else if (!slide_dir) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_body);
        lv_anim_set_exec_cb(&a, anim_ty_cb);
        lv_anim_set_values(&a, 36, 0);
        lv_anim_set_duration(&a, 240);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    if (s_nicons && !s_intro_tm) {
        s_intro_t0 = esp_timer_get_time();
        s_intro_tm = lv_timer_create(intro_tick, 33, NULL);
    }
}

// ------------------------------------------------------------------------------------- templates

static void build_menu(const cJSON *scr)
{
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(scr, "items")) {
        const char *bg = jstr(it, "bg");
        lv_obj_t *t = box(s_body, color_of(bg, C_CARD), 24);
        lv_obj_set_size(t, 140, 164);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(C_LINE), 0);
        make_clickable(t, 'i', jstr(it, "id"), jstr(it, "confirm"));
        // ícone: "vox" (modelo) > "icon" (nome do DashDeck) > bloco na cor do item
        const char *model = jstr(it, "vox");
        if (!model) model = vox_for_icon(jstr(it, "icon"));
        uint32_t tint = color_of(jstr(it, "color"), 0);
        lv_obj_t *cv;
        const char *img = jstr(it, "img");
        const lv_image_dsc_t *ic = NULL;
        char src[112] = "";
        if (img && sd_ok()) {
            size_t L = strlen(img);
            if (L > 4 && !strcmp(img + L - 4, ".bin")) {
                ic = icon_cached(img);   // ícone do dashd (na PSRAM depois da 1.ª vez)
            } else {                     // PNG/JPG posto à mão no cartão: só se existir (senão a box ficava vazia)
                snprintf(src, sizeof(src), "S:%s", img);
                lv_fs_file_t fh;
                if (lv_fs_open(&fh, src, LV_FS_MODE_RD) == LV_FS_RES_OK) lv_fs_close(&fh);
                else src[0] = '\0';
            }
        }
        if (ic || src[0]) {
            cv = lv_image_create(t);
            if (ic) lv_image_set_src(cv, ic);
            else lv_image_set_src(cv, src);
            lv_obj_set_size(cv, 84, 84);
            lv_image_set_inner_align(cv, LV_IMAGE_ALIGN_CONTAIN);
        } else {
            cv = vox_icon(t, model, tint, 84);
        }
        lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        const char *val = jstr(it, "value");
        lv_obj_t *l = label(t, jstr(it, "label"), &deck_font_20, C_TEXT);
        lv_obj_set_width(l, 128);
        lv_obj_set_height(l, lv_font_get_line_height(&deck_font_20));   // uma linha (com altura livre, o LVGL quebra)
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, val ? 88 : 96);
        if (val) {
            lv_obj_t *v = label(t, val, &deck_font_20, color_of(jstr(it, "value_color"), C_ACCENT));
            lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 112);
        }
        lv_obj_t *s = label(t, jstr(it, "sub"), &deck_font_16, color_of(jstr(it, "sub_color"), C_SUB));
        lv_obj_set_width(s, 128);
        lv_obj_set_height(s, lv_font_get_line_height(&deck_font_16));
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, val ? 138 : 124);
        int badge = jint(it, "badge", 0);
        if (badge > 0) {
            lv_obj_t *b = box(t, C_ERR, LV_RADIUS_CIRCLE);
            lv_obj_set_size(b, 32, 32);
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -6, 6);
            lv_obj_t *bl = label(b, "", &deck_font_16, 0xFFFFFF);
            lv_label_set_text_fmt(bl, "%d", badge > 99 ? 99 : badge);
            lv_obj_center(bl);
        }
        const char *dot = jstr(it, "dot");
        if (dot) {
            lv_obj_t *d = box(t, color_of(dot, C_OK), LV_RADIUS_CIRCLE);
            lv_obj_set_size(d, 12, 12);
            lv_obj_align(d, LV_ALIGN_TOP_RIGHT, -12, 12);
        }
    }
}

static void build_list(const cJSON *scr)
{
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    const cJSON *items = cJSON_GetObjectItem(scr, "items");
    if (cJSON_GetArraySize(items) == 0) {
        const char *empty = jstr(scr, "empty");
        lv_obj_t *l = label(s_body, empty ? empty : "Vazio", &deck_font_20, C_SUB);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 60, 0);
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        lv_obj_t *c = box(s_body, color_of(jstr(it, "bg"), C_CARD), 18);
        lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(c, 18, 0);
        lv_obj_set_style_pad_ver(c, 12, 0);
        lv_obj_set_style_min_height(c, 64, 0);
        if (jstr(it, "id")) make_clickable(c, 'i', jstr(it, "id"), jstr(it, "confirm"));
        const char *right = jstr(it, "right");
        lv_obj_t *l = label(c, jstr(it, "label"), &deck_font_20, color_of(jstr(it, "color"), C_TEXT));
        lv_obj_set_width(l, right ? 300 : 400);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        const char *sub = jstr(it, "sub");
        if (sub) {
            lv_obj_t *s = label(c, sub, &deck_font_16, C_SUB);
            lv_obj_set_width(s, right ? 300 : 400);
            lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
            lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 28);
        }
        if (right) {
            lv_obj_t *r = label(c, right, &deck_font_20, C_SUB);
            lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

// cartão chave-valor em parent; w = largura (lv_pct(100) ou px). As colunas ajustam-se à largura.
static void build_kv_in(lv_obj_t *parent, const cJSON *scr, int32_t w)
{
    if (cJSON_GetArraySize(cJSON_GetObjectItem(scr, "rows")) == 0) return;
    lv_obj_t *c = box(parent, C_CARD, 18);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    bool narrow = w > 0 && w < 300 && !LV_COORD_IS_PCT(w);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(scr, "rows")) {
        lv_obj_t *r = box(c, C_CARD, 0);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        const char *col = cJSON_GetStringValue(cJSON_GetArrayItem(row, 2));
        if (narrow) {   // ao lado de um plot: chave por cima, valor por baixo
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
            lv_obj_t *k = label(r, cJSON_GetStringValue(cJSON_GetArrayItem(row, 0)), &deck_font_16, C_SUB);
            lv_obj_set_width(k, lv_pct(100));
            lv_label_set_long_mode(k, LV_LABEL_LONG_DOT);
            lv_obj_t *v = label(r, cJSON_GetStringValue(cJSON_GetArrayItem(row, 1)), &deck_font_20, color_of(col, C_TEXT));
            lv_obj_set_width(v, lv_pct(100));
            lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
            continue;
        }
        lv_obj_t *k = label(r, cJSON_GetStringValue(cJSON_GetArrayItem(row, 0)), &deck_font_20, C_SUB);
        lv_obj_set_width(k, 170);
        lv_label_set_long_mode(k, LV_LABEL_LONG_DOT);
        lv_obj_t *v = label(r, cJSON_GetStringValue(cJSON_GetArrayItem(row, 1)), &deck_font_20, color_of(col, C_TEXT));
        lv_obj_set_width(v, 230);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
}

static void build_kv(const cJSON *scr)
{
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    build_kv_in(s_body, scr, lv_pct(100));
}

// ---- "plot" (do DashDeck): canvas + linhas kv ao lado (se couber) ou por baixo
static const char *ui_screen_id(void)
{
    return s_id;
}

static void plot_send_btn(const char *b)
{
    send_tap("btn", "btn", b);
}

static void plot_send_swipe(const char *d)
{
    send_tap("swipe", "dir", d);
}

static void rebuild_async(void *arg);

static void plot_drag(bool on)
{
    s_dragging = on;
    if (!on && s_rebuild_pending) lv_async_call(rebuild_async, NULL);
}

static void build_plot(const cJSON *scr)
{
    static const plot_env_t env = {.alloc = buf_add, .color = color_of, .screen_id = ui_screen_id,
                                   .send_btn = plot_send_btn, .send_swipe = plot_send_swipe, .drag = plot_drag};
    const cJSON *p = cJSON_GetObjectItem(scr, "plot");
    int32_t pw = cJSON_IsNumber(cJSON_GetObjectItem(p, "w")) ? cJSON_GetObjectItem(p, "w")->valueint : 160;
    int32_t rest = 444 - pw - 10;
    bool beside = rest >= 150 && cJSON_GetArraySize(cJSON_GetObjectItem(scr, "rows")) > 0;
    lv_obj_set_flex_flow(s_body, beside ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_body, 10, 0);
    plot_create(s_body, p, &env);
    if (beside) build_kv_in(s_body, scr, rest);
    else build_kv_in(s_body, scr, lv_pct(100));
}

// ---- "log": linhas de texto (as mais recentes em baixo); num update, "append" acrescenta
static void build_log(const cJSON *scr)
{
    lv_obj_t *c = box(s_body, C_CARD, 18);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 14, 0);
    char *txt = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!txt) return;
    size_t len = 0;
    txt[0] = '\0';
    const cJSON *ln;
    cJSON_ArrayForEach(ln, cJSON_GetObjectItem(scr, "lines")) {
        if (!cJSON_IsString(ln)) continue;
        int w = snprintf(txt + len, 8192 - len, "%s%s", len ? "\n" : "", ln->valuestring);
        if (w < 0 || len + w >= 8191) break;
        len += w;
    }
    lv_obj_t *l = label(c, txt, &deck_font_16, C_TEXT);
    heap_caps_free(txt);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(s_body);
    lv_obj_scroll_to_y(s_body, LV_COORD_MAX, LV_ANIM_OFF);
}

static void build_text(const cJSON *scr)
{
    lv_obj_t *l = label(s_body, jstr(scr, "text"), &deck_font_20, C_TEXT);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

static void build_buttons(const cJSON *scr)
{
    const cJSON *btns = cJSON_GetObjectItem(scr, "buttons");
    if (cJSON_GetArraySize(btns) == 0) return;
    lv_obj_t *row = box(s_body, C_BG, 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_set_style_pad_top(row, 8, 0);
    const cJSON *b;
    cJSON_ArrayForEach(b, btns) {
        uint32_t col = color_of(jstr(b, "color"), C_ACCENT);
        lv_obj_t *o = box(row, col, 18);
        lv_obj_set_size(o, LV_SIZE_CONTENT, 60);
        lv_obj_set_style_min_width(o, 130, 0);
        lv_obj_set_flex_grow(o, 1);
        lv_obj_set_style_pad_hor(o, 18, 0);
        make_clickable(o, 'b', jstr(b, "id"), jstr(b, "confirm"));
        lv_obj_set_style_bg_color(o, lv_color_hex(col), LV_STATE_PRESSED);
        lv_obj_center(label(o, jstr(b, "label"), &deck_font_20, 0x061018));
    }
}

// ---- sliders: "sliders": [{id, label, v, min, max, step, unit, color, live}]
// Arrastar mostra o valor; ao largar envia {"t":"slider","id":<tela>,"item":<id>,"v":n}.
// live = "bl" aplica o brilho enquanto se arrasta (resposta imediata, sem esperar pela rede).
typedef struct {
    char id[24];
    char unit[8];
    char live[8];
    int step;
    lv_obj_t *val;
} slider_ctx_t;

static void build_body(const cJSON *scr);
static void clock_apply(const cJSON *scr);
static void clock_hide(void);

static void rebuild_async(void *arg)
{
    if (!s_rebuild_pending || !s_screen) return;
    s_rebuild_pending = false;
    int32_t sy = lv_obj_get_scroll_y(s_body);
    build_body(s_screen);
    lv_obj_update_layout(s_body);
    lv_obj_scroll_to_y(s_body, sy, LV_ANIM_OFF);
}

static void slider_cb(lv_event_t *e)
{
    slider_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    if (c->step > 1) {
        v = (v + c->step / 2) / c->step * c->step;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        return;
    }
    lv_label_set_text_fmt(c->val, "%d%s", v, c->unit);
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (!strcmp(c->live, "bl")) bsp_display_brightness_set(v < 2 ? 2 : v);
        return;
    }
    // largado
    s_dragging = false;
    lv_slider_set_value(sl, v, LV_ANIM_OFF);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "slider");
    cJSON_AddStringToObject(o, "id", s_id);
    cJSON_AddStringToObject(o, "item", c->id);
    cJSON_AddNumberToObject(o, "v", v);
    link_send(o);
    cJSON_Delete(o);
    if (s_rebuild_pending) lv_async_call(rebuild_async, NULL);   // não apagar o slider dentro do seu evento
}

static void build_sliders(const cJSON *scr)
{
    const cJSON *arr = cJSON_GetObjectItem(scr, "sliders");
    if (cJSON_GetArraySize(arr) == 0) return;
    lv_obj_t *c = box(s_body, C_CARD, 18);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 22, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        slider_ctx_t *ctx = ref_mem(sizeof(slider_ctx_t));
        if (!ctx) break;
        strlcpy(ctx->id, jstr(it, "id") ? jstr(it, "id") : "", sizeof(ctx->id));
        strlcpy(ctx->unit, jstr(it, "unit") ? jstr(it, "unit") : "", sizeof(ctx->unit));
        strlcpy(ctx->live, jstr(it, "live") ? jstr(it, "live") : "", sizeof(ctx->live));
        ctx->step = jint(it, "step", 1);
        int mn = jint(it, "min", 0), mx = jint(it, "max", 100), v = jint(it, "v", mn);
        uint32_t col = color_of(jstr(it, "color"), C_ACCENT);

        lv_obj_t *r = box(c, C_CARD, 0);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        label(r, jstr(it, "label"), &deck_font_20, C_SUB);
        ctx->val = label(r, "", &deck_font_20, C_TEXT);
        lv_label_set_text_fmt(ctx->val, "%d%s", v, ctx->unit);
        lv_obj_align(ctx->val, LV_ALIGN_TOP_RIGHT, 0, 0);

        lv_obj_t *sl = lv_slider_create(c);
        lv_obj_set_size(sl, lv_pct(94), 16);
        lv_obj_set_style_margin_left(sl, 10, 0);
        lv_slider_set_range(sl, mn, mx);
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(C_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(col), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
        lv_obj_set_ext_click_area(sl, 18);
        lv_obj_remove_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);   // arrastar o slider não muda de página
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, ctx);
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_RELEASED, ctx);
        lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_PRESS_LOST, ctx);
    }
}

// ---- gráficos: "chart": {title, min, max, h, labels:[esq, dir], series:[{color, points:[...]}]}
// (ou uma lista destes objetos). Linhas sobre fundo preto, como o monitor de bateria.
static void build_one_chart(const cJSON *ch)
{
    lv_obj_t *c = box(s_body, C_CARD, 18);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    int mn = jint(ch, "min", 0), mx = jint(ch, "max", 100);
    lv_obj_t *head = box(c, C_CARD, 0);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    label(head, jstr(ch, "title"), &deck_font_20, C_SUB);
    if (jstr(ch, "value")) {
        lv_obj_t *v = label(head, jstr(ch, "value"), &deck_font_20, color_of(jstr(ch, "value_color"), C_TEXT));
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    lv_obj_t *row = box(c, C_CARD, 0);
    lv_obj_set_size(row, lv_pct(100), jint(ch, "h", 150));
    lv_obj_t *ymax = label(row, "", &deck_font_16, C_SUB);
    lv_label_set_text_fmt(ymax, "%d", mx);
    lv_obj_align(ymax, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_t *ymin = label(row, "", &deck_font_16, C_SUB);
    lv_label_set_text_fmt(ymin, "%d", mn);
    lv_obj_align(ymin, LV_ALIGN_BOTTOM_LEFT, 0, 4);

    lv_obj_t *chart = lv_chart_create(row);
    lv_obj_set_size(chart, lv_pct(86), lv_pct(100));
    lv_obj_align(chart, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 8, 0);
    lv_obj_set_style_pad_all(chart, 4, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);   // sem pontos, só a linha
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);

    const cJSON *series = cJSON_GetObjectItem(ch, "series");
    int npts = 2;
    const cJSON *se;
    cJSON_ArrayForEach(se, series) {
        int n = cJSON_GetArraySize(cJSON_GetObjectItem(se, "points"));
        if (n > npts) npts = n;
    }
    lv_chart_set_point_count(chart, npts);
    cJSON_ArrayForEach(se, series) {
        lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(color_of(jstr(se, "color"), C_ACCENT)),
                                                     LV_CHART_AXIS_PRIMARY_Y);
        const cJSON *pts = cJSON_GetObjectItem(se, "points");
        int n = cJSON_GetArraySize(pts), i = 0;
        // alinhada à direita: a série mais curta acaba em "agora"
        for (int k = 0; k < npts - n; k++) lv_chart_set_next_value(chart, ser, LV_CHART_POINT_NONE);
        const cJSON *p;
        cJSON_ArrayForEach(p, pts) {
            lv_chart_set_next_value(chart, ser, cJSON_IsNumber(p) ? (int32_t)p->valuedouble : LV_CHART_POINT_NONE);
            i++;
        }
    }
    lv_chart_refresh(chart);

    const cJSON *lbls = cJSON_GetObjectItem(ch, "labels");
    if (cJSON_GetArraySize(lbls) >= 2) {
        lv_obj_t *xr = box(c, C_CARD, 0);
        lv_obj_set_size(xr, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_t *a = label(xr, cJSON_GetStringValue(cJSON_GetArrayItem(lbls, 0)), &deck_font_16, C_SUB);
        lv_obj_align(a, LV_ALIGN_LEFT_MID, 48, 0);
        lv_obj_t *b = label(xr, cJSON_GetStringValue(cJSON_GetArrayItem(lbls, cJSON_GetArraySize(lbls) - 1)),
                            &deck_font_16, C_SUB);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void build_charts(const cJSON *scr)
{
    const cJSON *ch = cJSON_GetObjectItem(scr, "chart");
    if (cJSON_IsObject(ch)) build_one_chart(ch);
    else if (cJSON_IsArray(ch)) {
        const cJSON *one;
        cJSON_ArrayForEach(one, ch) build_one_chart(one);
    }
}

// ---- "image": {"src": "scenes/x.jpg", "caption": "..."} (ficheiro do cartão, PNG ou JPG)
static void build_image(const cJSON *scr)
{
    char src[112];
    snprintf(src, sizeof(src), "S:%s", jstr(scr, "src") ? jstr(scr, "src") : "");
    lv_obj_t *im = lv_image_create(s_body);
    lv_image_set_src(im, src);
    lv_obj_set_size(im, lv_pct(100), jint(scr, "h", 320));
    lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CONTAIN);
    if (jstr(scr, "caption")) {
        lv_obj_t *l = label(s_body, jstr(scr, "caption"), &deck_font_20, C_SUB);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
}

// ---- "vox": {"model": "nome", "caption": "...", "spin": true} cena em voxels (até 24x24x24, do cartão
// ou do host) que se monta voxel a voxel e depois roda devagar; o IMU inclina-a
static void scene_tick(lv_timer_t *t)
{
    if (!s_scene_cv || !s_scene_m) return;
    float sec = (esp_timer_get_time() - s_scene_t0) / 1e6f;
    float ft = vox_fly_time(s_scene_m);
    vox_view_t v = VOX_VIEW_DEFAULT;
    v.fly = sec < ft + 0.1f ? sec * 1.5f + 1 : 0;
    v.yaw = 0.785f + (sec > ft / 1.5f ? (sec - ft / 1.5f) * 0.3f : 0);
    v.pitch = 0.6f + imu_tilt();
    memset(s_scene_buf, 0, SCENE_W * SCENE_H * 4);
    vox_render(s_scene_m, &v, s_scene_buf, SCENE_W, SCENE_H, canvas_stride(s_scene_cv));
    lv_obj_invalidate(s_scene_cv);
}

static void build_scene(const cJSON *scr)
{
    s_scene_m = vox_get(jstr(scr, "model"));
    if (!s_scene_m) {
        lv_obj_t *l = label(s_body, "Modelo não encontrado (deck3/vox/<nome>.json no cartão)", &deck_font_20, C_WARN);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return;
    }
    s_scene_buf = buf_add(SCENE_W * SCENE_H * 4);
    if (!s_scene_buf) return;
    s_scene_cv = lv_canvas_create(s_body);
    lv_canvas_set_buffer(s_scene_cv, s_scene_buf, SCENE_W, SCENE_H, LV_COLOR_FORMAT_ARGB8888);
    if (jstr(scr, "caption")) {
        lv_obj_t *l = label(s_body, jstr(scr, "caption"), &deck_font_20, C_SUB);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    s_scene_t0 = esp_timer_get_time();
    scene_tick(NULL);
    if (!cJSON_IsFalse(cJSON_GetObjectItem(scr, "spin"))) s_scene_tm = lv_timer_create(scene_tick, 50, NULL);
}

static void build_body(const cJSON *scr)
{
    body_free();
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_body, 12, 0);
    const char *tpl = jstr(scr, "tpl");
    if (!tpl) tpl = "text";
    build_sliders(scr);   // extras de qualquer template: sliders e gráficos antes do corpo
    build_charts(scr);
    if (strcmp(tpl, "clock") != 0) clock_hide();
    if (!strcmp(tpl, "panel")) { /* só os extras */ }
    else if (!strcmp(tpl, "clock")) clock_apply(scr);
    else if (!strcmp(tpl, "menu")) build_menu(scr);
    else if (!strcmp(tpl, "image")) build_image(scr);
    else if (!strcmp(tpl, "plot")) build_plot(scr);
    else if (!strcmp(tpl, "log")) build_log(scr);
    else if (!strcmp(tpl, "vox")) build_scene(scr);
    else if (!strcmp(tpl, "list")) build_list(scr);
    else if (!strcmp(tpl, "kv")) build_kv(scr);
    else if (!strcmp(tpl, "text")) build_text(scr);
    else {
        char msg[80];
        snprintf(msg, sizeof(msg), "Template \"%s\" ainda não existe no Deck³", tpl);
        lv_obj_t *l = label(s_body, msg, &deck_font_20, C_WARN);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    build_buttons(scr);
}

// Mostra s_screen. fresh = tela nova (com animação de entrada).
static void apply_screen(bool fresh)
{
    const cJSON *msg = s_screen;
    const char *id = jstr(msg, "id");
    bool same = id && !strcmp(id, s_id);
    int page = jint(msg, "page", 0);
    int dir = same && s_page >= 0 && page != s_page ? (page > s_page ? 1 : -1) : 0;
    strlcpy(s_id, id ? id : "", sizeof(s_id));
    s_page = page;
    if (cJSON_IsFalse(cJSON_GetObjectItem(msg, "back"))) lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    set_title(jstr(msg, "title"));
    set_dots(jint(msg, "pages", 1), page);
    const char *bg = jstr(msg, "bg_img");   // imagem de fundo do cartão (ex. "scenes/praia.jpg")
    if (bg && sd_ok()) {
        char src[112];
        snprintf(src, sizeof(src), "S:%s", bg);
        lv_image_set_src(s_bgimg, src);
        lv_obj_remove_flag(s_bgimg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bgimg, LV_OBJ_FLAG_HIDDEN);
    }
    build_body(s_screen);
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    if (fresh) intro_start(jstr(msg, "tpl"), dir);
}

static void show_screen(const cJSON *msg)
{
    cJSON_Delete(s_screen);
    s_screen = cJSON_Duplicate(msg, true);
    if (s_boot_buf) {   // ecrã de arranque: a tela entra quando a sequência acabar
        s_boot_exit = true;
        return;
    }
    apply_screen(true);
}

static void update_screen(const cJSON *msg)
{
    const char *id = jstr(msg, "id");
    if (!s_screen || !id) return;
    if (s_boot_buf) {   // ainda no arranque: junta à tela pendente
        if (strcmp(id, jstr(s_screen, "id") ? jstr(s_screen, "id") : "") != 0) return;
    } else if (strcmp(id, s_id) != 0) {
        return;
    }
    const cJSON *app = cJSON_GetObjectItem(msg, "append");   // log: acrescenta (fica com as últimas 120)
    if (cJSON_IsArray(app)) {
        cJSON *lines = cJSON_GetObjectItem(s_screen, "lines");
        if (!cJSON_IsArray(lines)) lines = cJSON_AddArrayToObject(s_screen, "lines");
        const cJSON *ln;
        cJSON_ArrayForEach(ln, app) cJSON_AddItemToArray(lines, cJSON_Duplicate(ln, true));
        while (cJSON_GetArraySize(lines) > 120) cJSON_DeleteItemFromArray(lines, 0);
    }
    const cJSON *f;
    cJSON_ArrayForEach(f, msg) {
        if (!strcmp(f->string, "t") || !strcmp(f->string, "id") || !strcmp(f->string, "append")) continue;
        cJSON *dup = cJSON_Duplicate(f, true);
        if (cJSON_GetObjectItem(s_screen, f->string)) cJSON_ReplaceItemInObject(s_screen, f->string, dup);
        else cJSON_AddItemToObject(s_screen, f->string, dup);
    }
    if (s_boot_buf) return;
    if (cJSON_GetObjectItem(msg, "title")) set_title(jstr(s_screen, "title"));
    if (s_dragging) {   // não destruir o slider debaixo do dedo
        s_rebuild_pending = true;
        return;
    }
    int32_t sy = lv_obj_get_scroll_y(s_body);
    build_body(s_screen);
    lv_obj_update_layout(s_body);
    lv_obj_scroll_to_y(s_body, sy, LV_ANIM_OFF);
}

static void gesture_cb(lv_event_t *e)
{
    if (!s_id[0] || s_confirm || s_notif || s_dragging) return;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT) send_tap("swipe", "dir", "left");
    else if (d == LV_DIR_RIGHT) send_tap("swipe", "dir", "right");
}

// ------------------------------------------------------------------------------------ notificação

static void notif_close(void)
{
    if (s_notif_tm) lv_timer_delete(s_notif_tm);
    s_notif_tm = NULL;
    if (s_notif) lv_obj_delete(s_notif);
    s_notif = s_notif_cv = s_notif_band = NULL;
    heap_caps_free(s_notif_buf);
    s_notif_buf = NULL;
}

static void notif_tick(lv_timer_t *t)
{
    float el = (esp_timer_get_time() - s_notif_t0) / 1e6f;
    if (el > s_notif_total) {
        notif_close();
        return;
    }
    // fundo a escurecer e a clarear nas pontas (0,3 s)
    float fade = el < 0.3f ? el / 0.3f : el > s_notif_total - 0.3f ? (s_notif_total - el) / 0.3f : 1;
    lv_obj_set_style_bg_opa(s_notif, (lv_opa_t)(215 * fade), 0);
    anim_notify_draw(s_notif_buf, NCV_W, NCV_H, canvas_stride(s_notif_cv), el);
    lv_obj_invalidate(s_notif_cv);
    if (el > NOTIFY_ANIM) {   // a faixa sobe de baixo
        float p = (el - NOTIFY_ANIM) / 0.35f;
        p = p > 1 ? 1 : p;
        p = 1 - (1 - p) * (1 - p) * (1 - p);
        lv_obj_set_y(s_notif_band, H - (int32_t)(lv_obj_get_height(s_notif_band) * p));
    }
}

static void notif_tap(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);   // o toque não passa para a tela por baixo
    link_send_event("notify", "act", "open", NULL, NULL);
    notif_close();
}

static void notify_show(const cJSON *msg)
{
    notif_close();
    wake_nolock();
    const char *k = jstr(msg, "kind");
    int kind = k && !strcmp(k, "alarm") ? 1 : k && !strcmp(k, "squawk") ? 2 : 0;
    const vox_model_t *m = jstr(msg, "vox") ? vox_get(jstr(msg, "vox")) : NULL;
    bool tint = !m;   // os modelos de origem ganham a cor do tipo; os do host ficam como são
    if (!m) m = vox_get(kind == 1 ? "clock" : kind == 2 ? "plane" : "mail");
    s_notif_buf = heap_caps_calloc(1, NCV_W * NCV_H * 4, MALLOC_CAP_SPIRAM);
    if (!s_notif_buf) return;
    const char *from = jstr(msg, "from");
    anim_notify_start(m, from ? from : jstr(msg, "title"), kind, tint);

    s_notif = box(lv_layer_top(), 0x000000, 0);
    lv_obj_set_size(s_notif, W, H);
    lv_obj_set_style_bg_opa(s_notif, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_notif, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_notif, notif_tap, LV_EVENT_PRESSED, NULL);
    s_notif_cv = lv_canvas_create(s_notif);
    lv_canvas_set_buffer(s_notif_cv, s_notif_buf, NCV_W, NCV_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_align(s_notif_cv, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_remove_flag(s_notif_cv, LV_OBJ_FLAG_CLICKABLE);

    // faixa com o texto: sobe de baixo depois da animação (como no DashDeck)
    uint32_t light = anim_notify_color(kind, 1);
    s_notif_band = box(s_notif, 0x0B1016, 0);
    lv_obj_set_size(s_notif_band, W, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(s_notif_band, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_notif_band, 3, 0);
    lv_obj_set_style_border_color(s_notif_band, lv_color_hex(anim_notify_color(kind, 0)), 0);
    lv_obj_set_style_pad_hor(s_notif_band, 34, 0);
    lv_obj_set_style_pad_top(s_notif_band, 14, 0);
    lv_obj_set_style_pad_bottom(s_notif_band, 30, 0);
    lv_obj_set_flex_flow(s_notif_band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_notif_band, 4, 0);
    lv_obj_remove_flag(s_notif_band, LV_OBJ_FLAG_CLICKABLE);
    const char *title = jstr(msg, "title");
    char t[96];
    if (title) snprintf(t, sizeof(t), "%s", title);
    else snprintf(t, sizeof(t), "%s %s", kind == 1 ? "Alarme" : kind == 2 ? "Emergência" : "Mensagem de", from ? from : "");
    lv_obj_t *l1 = label(s_notif_band, t, &deck_font_28, light);
    lv_obj_set_width(l1, lv_pct(100));
    lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
    lv_obj_t *l2 = label(s_notif_band, jstr(msg, "text"), &deck_font_20, C_TEXT);
    lv_obj_set_width(l2, lv_pct(100));
    lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(l2, 110, 0);
    lv_obj_update_layout(s_notif_band);
    lv_obj_set_y(s_notif_band, H);

    int dur = jint(msg, "dur", 8);
    s_notif_total = dur > 0 && dur <= 300 ? (float)dur : 8;
    s_notif_t0 = esp_timer_get_time();
    s_notif_tm = lv_timer_create(notif_tick, 33, NULL);
    notif_tick(NULL);
    if (!cJSON_IsFalse(cJSON_GetObjectItem(msg, "sound"))) {
        if (kind == 1) {
            for (int i = 0; i < 3; i++) {
                audio_tone(988, 120, 0);
                audio_tone(0, 80, 0);
                audio_tone(988, 120, 0);
                audio_tone(0, 400, 0);
            }
        } else if (kind == 2) {
            audio_tone(660, 180, 0);
            audio_tone(440, 260, 0);
        } else {
            audio_tone(1047, 70, 0);
            audio_tone(0, 30, 0);
            audio_tone(1568, 110, 0);
        }
    }
}

// ------------------------------------------------------------------------------------ relógio
// tpl "clock" do dashd: ecrã inteiro com uma das faces de anim.c (a hora é a do relógio do cubo; o dashd dá a
// previsão). Swipe para os lados troca a face (guardada no cubo); um toque volta.
static lv_obj_t *s_clk, *s_clk_cv, *s_clk_name;
static uint32_t *s_clk_buf;
static lv_timer_t *s_clk_tm;
static int64_t s_clk_t0, s_clk_name_until;
static int s_clk_face;
static clock3_t s_clk_data;

static void clock_hide(void)
{
    if (s_clk_tm) lv_timer_delete(s_clk_tm);
    s_clk_tm = NULL;
    if (s_clk) lv_obj_delete(s_clk);
    s_clk = s_clk_cv = s_clk_name = NULL;
    heap_caps_free(s_clk_buf);
    s_clk_buf = NULL;
}

static void clock_tick(lv_timer_t *t)
{
    static const char *DOW[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
    static const char *MON[] = {"JAN", "FEV", "MAR", "ABR", "MAI", "JUN", "JUL", "AGO", "SET", "OUT", "NOV", "DEZ"};
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8], date[24];
    if (tm.tm_year > 120) {
        snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(date, sizeof(date), "%s %d %s", DOW[tm.tm_wday % 7], tm.tm_mday, MON[tm.tm_mon % 12]);
    } else {
        strcpy(hm, "--:--");
        date[0] = '\0';
    }
    power_info_t p;
    power_get(&p);
    s_clk_data.time = hm;
    s_clk_data.date = date;
    s_clk_data.bat = p.percent;
    s_clk_data.chg = p.charging;
    s_clk_data.shift = (int)((now / 60) % 9);
    float el = (esp_timer_get_time() - s_clk_t0) / 1e6f;
    anim_clock_draw(s_clk_face, s_clk_buf, W, H, canvas_stride(s_clk_cv), el, &s_clk_data);
    lv_obj_invalidate(s_clk_cv);
    if (s_clk_name && esp_timer_get_time() > s_clk_name_until) lv_obj_add_flag(s_clk_name, LV_OBJ_FLAG_HIDDEN);
}

static void clock_face_name(void)
{
    lv_label_set_text(s_clk_name, anim_clock_name(s_clk_face));
    lv_obj_remove_flag(s_clk_name, LV_OBJ_FLAG_HIDDEN);
    s_clk_name_until = esp_timer_get_time() + 1500000;
}

static void clock_ev(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;
        s_clk_face = (s_clk_face + (d == LV_DIR_LEFT ? 1 : ANIM_CLOCK_FACES - 1)) % ANIM_CLOCK_FACES;
        store_set("clkface", s_clk_face);
        clock_face_name();
        lv_indev_wait_release(lv_indev_active());   // o gesto não conta também como toque
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        send_tap("back", NULL, NULL);
    }
}

// Dados do "clock" do dashd: previsão e ícone do tempo ({"wx":{t,max,min,rain}, "icon":{pal,d}}).
static void clock_apply(const cJSON *scr)
{
    const cJSON *c = cJSON_GetObjectItem(scr, "clock");
    const cJSON *wx = cJSON_GetObjectItem(c, "wx");
    s_clk_data.has_wx = cJSON_IsObject(wx);
    if (s_clk_data.has_wx) {
        s_clk_data.temp = jint(wx, "t", 0);
        s_clk_data.tmax = jint(wx, "max", 0);
        s_clk_data.tmin = jint(wx, "min", 0);
        s_clk_data.rain = jint(wx, "rain", 0);
    }
    const cJSON *ic = cJSON_GetObjectItem(c, "icon");
    const char *d = jstr(ic, "d");
    const cJSON *pal = cJSON_GetObjectItem(ic, "pal");
    s_clk_data.has_icon = d && strlen(d) >= 256 && cJSON_IsArray(pal);
    if (s_clk_data.has_icon) {
        for (int i = 0; i < 256; i++) s_clk_data.icon[i] = (uint8_t)(d[i] - '0');
        for (int i = 0; i < 8; i++) s_clk_data.icon_pal[i] = color_of(cJSON_GetStringValue(cJSON_GetArrayItem(pal, i)), 0);
    }
    if (s_clk) return;
    s_clk_buf = heap_caps_calloc(1, W * H * 4, MALLOC_CAP_SPIRAM);
    if (!s_clk_buf) return;
    s_clk_face = (int)store_get("clkface", 0) % ANIM_CLOCK_FACES;
    s_clk = box(s_scr, 0x000000, 0);   // por cima do cabeçalho e do corpo; os avisos ficam por cima (camada de topo)
    lv_obj_set_size(s_clk, W, H);
    lv_obj_add_flag(s_clk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_clk, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_clk, clock_ev, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_clk, clock_ev, LV_EVENT_SHORT_CLICKED, NULL);
    s_clk_cv = lv_canvas_create(s_clk);
    lv_canvas_set_buffer(s_clk_cv, s_clk_buf, W, H, LV_COLOR_FORMAT_XRGB8888);
    lv_obj_remove_flag(s_clk_cv, LV_OBJ_FLAG_CLICKABLE);
    s_clk_name = label(s_clk, "", &deck_font_20, 0xFFFFFF);
    lv_obj_set_style_bg_color(s_clk_name, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_clk_name, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_clk_name, 14, 0);
    lv_obj_set_style_pad_ver(s_clk_name, 4, 0);
    lv_obj_set_style_radius(s_clk_name, 14, 0);
    lv_obj_align(s_clk_name, LV_ALIGN_BOTTOM_MID, 0, -14);
    clock_face_name();
    s_clk_t0 = esp_timer_get_time();
    s_clk_tm = lv_timer_create(clock_tick, 66, NULL);   // ~15 qps
    clock_tick(NULL);
}

// ------------------------------------------------------------------------------------ "Sobre"

static void about_close(void)
{
    if (s_about_tm) lv_timer_delete(s_about_tm);
    s_about_tm = NULL;
    if (s_about) lv_obj_delete(s_about);
    s_about = s_about_cv = s_about_txt = NULL;
    heap_caps_free(s_about_buf);
    s_about_buf = NULL;
}

static void about_tick(lv_timer_t *t)
{
    float el = (esp_timer_get_time() - s_about_t0) / 1e6f;
    if (el > 40) {
        about_close();
        return;
    }
    anim_about_draw(s_about_buf, canvas_stride(s_about_cv), el, imu_tilt());
    lv_obj_invalidate(s_about_cv);
    // créditos: aparecem com o nome e somem ao fim de 7 s, deixando só a animação
    float a = el < 3.2f ? 0 : el < 3.7f ? (el - 3.2f) / 0.5f : el < 10.2f ? 1 : el < 10.7f ? (10.7f - el) / 0.5f : 0;
    lv_obj_set_style_opa(s_about_txt, (lv_opa_t)(255 * a), 0);
}

static void about_tap(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);
    about_close();
}

static void about_show(void)
{
    if (s_about || s_notif) return;
    s_about_buf = heap_caps_calloc(1, W * H * 4, MALLOC_CAP_SPIRAM);
    if (!s_about_buf) return;
    s_about = box(lv_layer_top(), 0x000000, 0);
    lv_obj_set_size(s_about, W, H);
    lv_obj_add_flag(s_about, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_about, about_tap, LV_EVENT_PRESSED, NULL);
    s_about_cv = lv_canvas_create(s_about);
    lv_canvas_set_buffer(s_about_cv, s_about_buf, W, H, LV_COLOR_FORMAT_XRGB8888);
    lv_obj_remove_flag(s_about_cv, LV_OBJ_FLAG_CLICKABLE);
    s_about_txt = box(s_about, 0x000000, 0);
    lv_obj_set_style_bg_opa(s_about_txt, LV_OPA_TRANSP, 0);
    lv_obj_set_size(s_about_txt, W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_about_txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_about_txt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_about_txt, 4, 0);
    lv_obj_remove_flag(s_about_txt, LV_OBJ_FLAG_CLICKABLE);
    label(s_about_txt, "Deck\xC2\xB3 " DECK3_FW_VERSION " \xC2\xB7 o DashDeck elevado ao cubo", &deck_font_20, 0xFFFFFF);
    const cJSON *ln;
    cJSON_ArrayForEach(ln, s_about_lines) {
        if (cJSON_IsString(ln)) label(s_about_txt, ln->valuestring, &deck_font_16, 0xB8C2D0);
    }
    lv_obj_align(s_about_txt, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_opa(s_about_txt, LV_OPA_TRANSP, 0);
    anim_about_start(W, H);
    s_about_t0 = esp_timer_get_time();
    s_about_tm = lv_timer_create(about_tick, 33, NULL);
    about_tick(NULL);
}

static void title_tap(lv_event_t *e)
{
    if (!strcmp(s_id, "home")) about_show();
}

// ------------------------------------------------------------------------------------ captura do ecrã

// {"t":"shot"}: redesenha o ecrã inteiro e, a cada faixa enviada ao painel, manda os píxeis (RGB565, base64)
// {"t":"shotpx","x","y","w","h","d"}; no fim {"t":"shotend","w","h"}. O dashd monta o PNG (deckctl.py shot).
static bool s_shot_on;
static int s_shot_n;
static char s_shot_line[2600];

static void shot_ev(lv_event_t *e)
{
    if (!s_shot_on) return;
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *b = lv_display_get_buf_active(lv_event_get_target(e));
    if (!a || !b || !b->data) return;
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    uint32_t stride = b->header.stride ? b->header.stride : (uint32_t)w * 2;
    int rows = 1800 / (w * 2);
    if (rows < 1) rows = 1;
    static uint8_t raw[1800];
    for (int y = 0; y < h; y += rows) {
        int n = h - y < rows ? h - y : rows;
        int k = snprintf(s_shot_line, sizeof(s_shot_line), "{\"t\":\"shotpx\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"d\":\"",
                         (int)a->x1, (int)a->y1 + y, w, n);
        size_t rl = 0;
        for (int r = 0; r < n; r++) {
            memcpy(raw + rl, b->data + (size_t)(y + r) * stride, (size_t)w * 2);
            rl += (size_t)w * 2;
        }
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char *)s_shot_line + k, sizeof(s_shot_line) - k - 4, &olen, raw, rl);
        k += (int)olen;
        snprintf(s_shot_line + k, sizeof(s_shot_line) - k, "\"}");
        link_send_text(s_shot_line);
        s_shot_n++;
    }
}

static void shot_take(void)
{
    static bool hooked;
    lv_display_t *d = lv_display_get_default();
    if (!hooked) {
        lv_display_add_event_cb(d, shot_ev, LV_EVENT_FLUSH_START, NULL);
        hooked = true;
    }
    lv_obj_invalidate(lv_screen_active());
    s_shot_n = 0;
    s_shot_on = true;
    lv_refr_now(d);
    s_shot_on = false;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"shotend\",\"w\":%d,\"h\":%d}", W, H);
    link_send_text(buf);
    ESP_LOGI(TAG, "captura: %d blocos enviados (via %s)", s_shot_n, link_via_name(link_active()));
}

// ------------------------------------------------------------------------------------ mensagens

void ui_send_hello(link_via_t via)
{
    net_info_t ni;
    net_get_info(&ni);
    power_info_t p;
    power_get(&p);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "hello");
    cJSON_AddStringToObject(o, "dev", "deck3");
    cJSON_AddStringToObject(o, "fw", DECK3_FW_VERSION);
    cJSON_AddStringToObject(o, "id", net_device_id());
    cJSON_AddNumberToObject(o, "w", W);
    cJSON_AddNumberToObject(o, "h", H);
    cJSON_AddStringToObject(o, "via", link_via_name(via));
    cJSON_AddNumberToObject(o, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "bat", p.percent);
    cJSON_AddBoolToObject(o, "chg", p.charging);
    cJSON_AddNumberToObject(o, "rssi", ni.rssi);
    cJSON_AddStringToObject(o, "ip", ni.ip);
    cJSON_AddNumberToObject(o, "bl", s_bl);
    cJSON_AddNumberToObject(o, "vol", audio_get_vol());
    cJSON_AddBoolToObject(o, "sd", sd_ok());
    cJSON *caps = cJSON_AddArrayToObject(o, "caps");
    const char *cl[] = {"vox", "notify", "audio", "mic", "imu", "keys", "ota", "confirm", "slider", "chart", "bat", "wifi_scan", "about", "sd", "image", "scene", "away"};
    for (size_t i = 0; i < sizeof(cl) / sizeof(cl[0]); i++) cJSON_AddItemToArray(caps, cJSON_CreateString(cl[i]));
    link_send_via(o, via);
    cJSON_Delete(o);
}

static void send_stats(void)
{
    net_info_t ni;
    net_get_info(&ni);
    power_info_t p;
    power_get(&p);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "stats");
    cJSON_AddNumberToObject(o, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(o, "rssi", ni.rssi);
    cJSON_AddNumberToObject(o, "bat", p.percent);
    cJSON_AddNumberToObject(o, "mv", p.mv);
    cJSON_AddBoolToObject(o, "usb", p.usb);
    link_send(o);
    cJSON_Delete(o);
}

void ui_handle(const cJSON *msg, link_via_t via)
{
    const char *t = jstr(msg, "t");
    if (!t) return;
    // mensagens que não mexem no LVGL
    if (!strcmp(t, "ping")) { link_send_event("pong", NULL, NULL, NULL, NULL); return; }
    if (!strcmp(t, "hello?")) { ui_send_hello(via); return; }
    if (!strcmp(t, "stats")) { send_stats(); return; }
    if (!strcmp(t, "reboot")) {
        ui_toast("A reiniciar...", "info");
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
    }
    if (!strcmp(t, "imu")) { imu_handle(msg); return; }
    if (!strcmp(t, "vox")) {
        // "save": true guarda o modelo no cartão (deck3/vox/<id>.json): fica disponível sem reenvio
        if (vox_define(msg) && cJSON_IsTrue(cJSON_GetObjectItem(msg, "save")) && sd_ok()) {
            cJSON *c = cJSON_Duplicate(msg, true);
            cJSON_DeleteItemFromObject(c, "t");
            cJSON_DeleteItemFromObject(c, "save");
            char *txt = cJSON_PrintUnformatted(c);
            char rel[48];
            snprintf(rel, sizeof(rel), "vox/%s.json", jstr(msg, "id"));
            if (txt) sd_write_text(rel, txt);
            cJSON_free(txt);
            cJSON_Delete(c);
        }
        return;
    }
    if (!strcmp(t, "time")) {
        const char *tz = jstr(msg, "tz");
        if (tz) { setenv("TZ", tz, 1); tzset(); }
        const cJSON *ep = cJSON_GetObjectItem(msg, "epoch");
        if (cJSON_IsNumber(ep)) {
            struct timeval tv = {.tv_sec = (time_t)ep->valuedouble};
            settimeofday(&tv, NULL);
        }
        return;
    }
    if (audio_handle(msg)) return;

    if (!bsp_display_lock(2000)) {
        ESP_LOGW(TAG, "lock do LVGL ocupado; '%s' descartada", t);
        return;
    }
    if (!strcmp(t, "screen")) show_screen(msg);
    else if (!strcmp(t, "update")) update_screen(msg);
    else if (!strcmp(t, "shot")) shot_take();
    else if (!strcmp(t, "toast")) toast_nolock(jstr(msg, "text"), jstr(msg, "level"));
    else if (!strcmp(t, "notify")) notify_show(msg);
    else if (!strcmp(t, "status")) strlcpy(s_status, jstr(msg, "text") ? jstr(msg, "text") : "", sizeof(s_status));
    else if (!strcmp(t, "bl")) {
        s_bl = jint(msg, "v", s_bl);
        if (s_bl < 2) s_bl = 2;   // 0 apagaria o ecrã sem forma de o ver; para apagar há o "idle"
        if (s_bl > 100) s_bl = 100;
        bsp_display_brightness_set(s_bl);
        s_bl_state = 0;
        store_set("bl", s_bl);
    } else if (!strcmp(t, "idle")) {
        s_dim_s = jint(msg, "dim", s_dim_s);
        s_off_s = jint(msg, "off", s_off_s);
    } else if (!strcmp(t, "wake")) wake_nolock();
    else if (!strcmp(t, "about")) {   // {"t":"about","lines":[...],"show":true}
        const cJSON *l = cJSON_GetObjectItem(msg, "lines");
        if (cJSON_IsArray(l)) {
            cJSON_Delete(s_about_lines);
            s_about_lines = cJSON_Duplicate(l, true);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "show"))) about_show();
    }
    else if (!strcmp(t, "rot")) {
        // experimental: a rotação do toque ainda tem de ser confirmada na placa
        bsp_display_rotation_set((bsp_display_rotation_t)(jint(msg, "v", 0) & 3));
    } else if (!strcmp(t, "offline")) boot_show();
    bsp_display_unlock();
}

// PWR sem host: alterna o brilho localmente (20/50/80/100) e guarda-o
void ui_local_bl_cycle(void)
{
    static const int lv[] = {20, 50, 80, 100};
    int n = 0;
    for (int i = 0; i < 4; i++) if (s_bl >= lv[i]) n = i + 1;
    s_bl = lv[n % 4];
    if (!bsp_display_lock(500)) return;
    bsp_display_brightness_set(s_bl);
    s_bl_state = 0;
    char t[24];
    snprintf(t, sizeof(t), "Brilho %d%%", s_bl);
    toast_nolock(t, "info");
    bsp_display_unlock();
    s_bl_dirty = true;   // gravado no tick (esta função corre na tarefa power, com stack na PSRAM)
}

// Bateria fraca: aviso no próprio cubo (pode estar longe do uConsole) a 20%, 10% e 5%
static void battery_watch(void)
{
    power_info_t p;
    power_get(&p);
    if (p.percent < 0 || p.charging || p.usb) {
        s_bat_warned = 101;
        return;
    }
    static const int levels[] = {20, 10, 5};
    for (int i = 0; i < 3; i++) {
        if (p.percent <= levels[i] && s_bat_warned > levels[i]) {
            s_bat_warned = levels[i];
            char t[40];
            snprintf(t, sizeof(t), "Bateria fraca: %d%%", p.percent);
            wake_nolock();
            toast_nolock(t, levels[i] <= 10 ? "err" : "warn");
            audio_tone(660, 120, 0);
            audio_tone(440, 220, 0);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "t", "bat_low");
            cJSON_AddNumberToObject(o, "pct", p.percent);
            link_send(o);
            cJSON_Delete(o);
        }
    }
}

void ui_local_back(void)
{
    if (!bsp_display_lock(500)) return;
    if (s_confirm) confirm_close();
    else if (s_notif) notif_close();
    else if (s_about) about_close();
    bsp_display_unlock();
}

// ------------------------------------------------------------------------------------------ init

static void tick_1s(lv_timer_t *t)
{
    hdr_refresh();
    idle_check();
    battery_watch();
    if (s_bl_dirty) {
        s_bl_dirty = false;
        store_set("bl", s_bl);
    }
    // sem host há mais de 15 s: volta ao ecrã de arranque com o estado da ligação
    bool online = link_host_alive() || net_ws_connected();
    int64_t now = esp_timer_get_time();
    if (online) s_offline_since = 0;
    else if (!s_offline_since) s_offline_since = now;
    else if (s_id[0] && now - s_offline_since > 15000000) boot_show_mode(true);   // perdeu o uConsole
    // de volta: entrega os eventos guardados no cartão enquanto esteve fora
    if (link_host_alive() && sd_events_pending() > 0 && now - s_last_flush > 10000000) {
        s_last_flush = now;
        sd_events_flush();
    }
}

void ui_init(lv_display_t *disp)
{
    s_bl = store_get("bl", 80);
    bsp_display_lock(0);   // 0 = esperar o tempo que for preciso
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    s_hdr = box(s_scr, C_BG, 0);
    lv_obj_set_size(s_hdr, W, HDR_H);
    lv_obj_set_style_pad_hor(s_hdr, 22, 0);   // cantos redondos do painel
    s_back = box(s_hdr, C_BG, 14);
    lv_obj_set_size(s_back, 52, 48);
    lv_obj_align(s_back, LV_ALIGN_LEFT_MID, -4, 4);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_back, lv_color_hex(0x1A222D), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(s_back, LV_SYMBOL_LEFT, &deck_font_28, C_TEXT));
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    // título à esquerda (uma linha: título + subtítulo); à direita, em duas linhas, a hora com o ponto da
    // ligação e, por baixo, o Wi-Fi e a bateria
    s_tbox = box(s_hdr, C_BG, 0);
    lv_obj_set_height(s_tbox, 44);
    lv_obj_set_flex_flow(s_tbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_tbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_tbox, 10, 0);
    s_title = label(s_tbox, "", &deck_font_28, C_TEXT);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_CLICKABLE);   // na tela inicial abre o "Sobre"
    lv_obj_set_ext_click_area(s_title, 10);
    lv_obj_add_event_cb(s_title, title_tap, LV_EVENT_CLICKED, NULL);
    s_tsub = label(s_tbox, "", &deck_font_20, C_SUB);
    lv_label_set_long_mode(s_tsub, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_tsub, lv_font_get_line_height(&deck_font_20));
    lv_obj_set_flex_grow(s_tsub, 1);
    lv_obj_set_style_translate_y(s_tsub, 3, 0);   // na linha de base do título
    s_clock = label(s_hdr, "--:--", &deck_font_20, C_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, 0, 4);
    s_link = box(s_hdr, C_ERR, LV_RADIUS_CIRCLE);
    lv_obj_set_size(s_link, 8, 8);
    lv_obj_align_to(s_link, s_clock, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    s_bat = label(s_hdr, "", &deck_font_16, C_SUB);
    lv_obj_align(s_bat, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
    s_wifi = label(s_hdr, LV_SYMBOL_WIFI, &deck_font_16, 0x3A424D);
    // páginas: pontos num "comprimido" em baixo, ao centro, por cima do corpo
    s_dots = box(s_scr, C_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_dots, LV_OPA_80, 0);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_pad_hor(s_dots, 9, 0);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 6, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);

    s_bgimg = lv_image_create(s_scr);   // por baixo do corpo
    lv_obj_set_size(s_bgimg, W, H - HDR_H);
    lv_obj_set_pos(s_bgimg, 0, HDR_H);
    lv_image_set_inner_align(s_bgimg, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(s_bgimg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_bgimg, LV_OBJ_FLAG_CLICKABLE);
    s_body = lv_obj_create(s_scr);
    lv_obj_set_size(s_body, W, H - HDR_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, HDR_H);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_move_foreground(s_dots);   // os pontos das páginas ficam por cima do corpo
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_hor(s_body, 18, 0);
    lv_obj_set_style_pad_top(s_body, 6, 0);
    lv_obj_set_style_pad_bottom(s_body, 28, 0);
    lv_obj_set_style_pad_gap(s_body, 12, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    boot_show();
    lv_timer_create(tick_1s, 1000, NULL);
    hdr_refresh();
    bsp_display_unlock();
    bsp_display_brightness_set(s_bl);
}
