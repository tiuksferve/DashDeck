// Interface do DashDeck³. Estrutura do ecrã (480×480, AMOLED: fundo preto puro = píxeis apagados):
//   cabeçalho (56 px): ← · título · pontos de página · hora · Wi-Fi · bateria
//   corpo: tela do host (menu em boxes com ícones, list, kv, text, botões) ou o ecrã de arranque
//   camada de cima: avisos (toast), confirmações, notificações em pixel art, progresso, painel de acordar
#include "ui.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR: tabelas grandes na PSRAM (a RAM interna é pouca)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "anim.h"
#include "bg.h"
#include "audio.h"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "deck.h"
#include "deck_fonts.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "imu.h"
#include "link.h"
#include "mbedtls/base64.h"
#include "net.h"
#include "plot.h"
#include "power.h"
#include "sd.h"
#include "store.h"
#include "varanda.h"
#include "src/misc/cache/instance/lv_image_cache.h"   // lv_image_cache_drop (emojis)
#include "src/misc/lv_text_private.h"                // lv_text_get_encoded_length (contador do teclado)

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
EXT_RAM_BSS_ATTR static char *s_refs[MAX_REFS];    // ids dos itens/botões (user_data dos eventos)
static int s_nrefs;
static void *s_bufs[MAX_BUFS];    // buffers dos canvas do corpo
static int s_nbufs;
static char s_status[64];         // texto do host para o cabeçalho (ex.: bateria do uConsole)

// quadros desenhados noutra tarefa (ver fr_open, mais abaixo)
typedef struct fr fr_t;
typedef void (*fr_fn_t)(fr_t *f, uint32_t *buf, int stride);
enum { FR_IDLE, FR_BUSY, FR_READY };
struct fr {
    lv_obj_t *cv;
    uint16_t *buf[2];
    int w, h, stride, front;
    fr_fn_t fn;
    volatile int state;
    float t, last;        // instante a desenhar; o último pedido
    union {               // cópia dos dados do quadro (o LVGL continua a mexer nos originais)
        struct { clock3_t c; char hm[8], date[24]; int face; } clk;
        struct { bool away; float prog, tilt; char st[80]; away_t a; char hm[8], date[24]; } boot;
    } a;
};

// arranque / offline: ecrã inteiro por baixo do cabeçalho, desenhado em anim.c
#define BOOT_W W
#define BOOT_H (H - HDR_H)
static lv_obj_t *s_boot_cv;
static fr_t s_boot_fr;
static lv_timer_t *s_boot_tm;
static int64_t s_boot_t0;
static bool s_boot_first = true;   // o 1.º arranque mostra a sequência toda (como no DashDeck)
static bool s_boot_min;            // este ecrã de arranque tem de passar a sequência toda
static bool s_boot_exit;           // chegou a 1.ª tela: fecha a barra e sai
static float s_boot_prog, s_boot_exit_t;
static bool s_away;                // modo "fora de casa": relógio em vez da animação (sem o uConsole)
static int64_t s_last_flush;

// imagem de fundo da tela ("bg")
static lv_obj_t *s_bgimg;

// entrada das telas: boxes a subir em cascata, página a deslizar
static int s_page = -1;

// "Sobre o DashDeck³" (toque no título da tela inicial)
static lv_obj_t *s_about, *s_about_txt;
static lv_timer_t *s_about_tm;
static int64_t s_about_t0;
static cJSON *s_about_lines;       // linhas de créditos enviadas pelo deckd ({"t":"about"})

// ecrã apagado
static int s_dim_s = 60, s_off_s = 300, s_bl = 80;
static int s_bl_state;            // 0 normal, 1 escurecido, 2 apagado (inatividade), 3 desligado no botão (0%)
static lv_obj_t *s_wake_pane;

// notificação ("Ondas LoRa" em pixel art)
#define NCV_W 360
#define NCV_H 320
static lv_obj_t *s_notif, *s_notif_cv, *s_notif_band;
static uint32_t *s_notif_buf;
static lv_timer_t *s_notif_tm;
static lv_obj_t *s_pomo;          // ecrã do Pomodoro (cubo girado para a direita)
static lv_obj_t *s_var;           // varanda do Romeu (cubo girado para a esquerda)
static bool s_var_want;           // o cubo está girado para a esquerda (abre quando houver imagens)
static bool s_rot_test;           // aberto por mensagem de teste: o IMU em pé não o fecha
static bool s_notif_local;       // alarme/temporizador do próprio cubo: o toque só cala, não vai ao host
static int64_t s_ring_until;     // o cubo toca o alarme na sua coluna até aqui (ou até um toque)
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

// ------------------------------------------------------------------------------------------ emojis
// Como no DashDeck: o dashd troca cada emoji (ou sequência: bandeira, tom de pele, ZWJ) por um código privado
// U+100000+i e envia antes a imagem ({"t":"emoji"}, RGB565A8). Uma imgfont do LVGL no fallback das fontes
// deck_font_* desenha esses códigos com a imagem guardada aqui (PSRAM).
#define EMOJI_MAX 48
#define EMOJI_CP0 0x100000
typedef struct {
    uint32_t cp;
    lv_image_dsc_t dsc;
    uint8_t *data;
} emoji_t;
EXT_RAM_BSS_ATTR static emoji_t s_emoji[EMOJI_MAX];

static const void *emoji_cb(const lv_font_t *font, uint32_t unicode, uint32_t next, int32_t *offset_y, void *ud)
{
    (void)font, (void)next;
    if (unicode < EMOJI_CP0 || unicode >= EMOJI_CP0 + EMOJI_MAX) return NULL;
    emoji_t *e = &s_emoji[unicode - EMOJI_CP0];
    if (!e->data || e->cp != unicode) return NULL;
    *offset_y = (int32_t)(intptr_t)ud;   // assenta na linha de base da fonte
    return &e->dsc;
}

static void emoji_fonts_init(void)
{
    // a imagem tem 18 px; a deslocação acerta-a à linha de base de cada tamanho
    static const struct { lv_font_t *f; int8_t dy; } fonts[] = {
        {&deck_font_16, -3}, {&deck_font_20, -4}, {&deck_font_28, -5}, {&deck_font_40, -8}};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        lv_font_t *im = lv_imgfont_create(18, emoji_cb, (void *)(intptr_t)fonts[i].dy);
        if (!im) continue;
        im->fallback = fonts[i].f->fallback;   // depois dos emojis, os símbolos LV_SYMBOL_*
        fonts[i].f->fallback = im;
    }
}

// {"t":"emoji","cp":0x100000+i,"w","h","d":base64 RGB565A8} -> {"t":"emojiack","cp","ok"}. Com o lock do LVGL.
static void handle_emoji(const cJSON *msg)
{
    int cp = jint(msg, "cp", 0), w = jint(msg, "w", 0), h = jint(msg, "h", 0);
    bool ok = false;
    if (cp >= EMOJI_CP0 && cp < EMOJI_CP0 + EMOJI_MAX && w > 0 && h > 0 && w <= 32 && h <= 32) {
        size_t size = (size_t)w * h * 3, olen = 0;
        uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        const char *d = jstr(msg, "d") ? jstr(msg, "d") : "";
        if (buf && mbedtls_base64_decode(buf, size, &olen, (const unsigned char *)d, strlen(d)) == 0 && olen == size) {
            emoji_t *e = &s_emoji[cp - EMOJI_CP0];
            if (e->data) {
                lv_image_cache_drop(&e->dsc);
                heap_caps_free(e->data);
            }
            memset(&e->dsc, 0, sizeof(e->dsc));
            e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            e->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
            e->dsc.header.w = w;
            e->dsc.header.h = h;
            e->dsc.header.stride = w * 2;
            e->dsc.data_size = size;
            e->dsc.data = buf;
            e->data = buf;
            e->cp = (uint32_t)cp;
            ok = true;
        } else {
            heap_caps_free(buf);
        }
    }
    char b[64];
    snprintf(b, sizeof(b), "{\"t\":\"emojiack\",\"cp\":%d,\"ok\":%s}", cp, ok ? "true" : "false");
    link_send_text(b);
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
    for (int i = 0; i < s_nrefs; i++) free(s_refs[i]);
    s_nrefs = 0;
    plot_detach();
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

// Telas inteiras opacas na camada de topo (Sobre, Pomodoro): o LVGL desenharia sempre a tela ativa por
// baixo, a cada quadro. Enquanto estão abertas, a tela ativa passa a ser uma vazia.
// Brilho: o comando vai pelo mesmo SPI que os quadros. Só a tarefa do LVGL o envia (entre refrescos);
// de outra tarefa, a meio de um envio ao painel, o driver SPI baralhava-se e o LVGL ficava à espera.
static volatile int s_bl_pending = -1;
static void bl_set(int v)
{
    s_bl_pending = v;
}

static void bl_apply_cb(lv_timer_t *t)
{
    int v = s_bl_pending;
    if (v < 0) return;
    s_bl_pending = -1;
    bsp_display_brightness_set(v);
}

static lv_obj_t *s_blank;
static uint32_t s_pf_dn;    // desenhos de quadros (perf)
static int64_t s_pf_draw;   // tempo total desses desenhos (us)
static void screen_cover(void)
{
    bool want = s_about || s_pomo || s_var;
    if (want && !s_blank) {
        s_blank = lv_obj_create(NULL);
        lv_obj_remove_style_all(s_blank);
        lv_obj_set_style_bg_color(s_blank, lv_color_hex(0), 0);
        lv_obj_set_style_bg_opa(s_blank, LV_OPA_COVER, 0);
    }
    lv_obj_t *act = lv_screen_active();
    if (want && act != s_blank) lv_screen_load(s_blank);
    else if (!want && act == s_blank) lv_screen_load(s_scr);
}

static int canvas_stride(lv_obj_t *cv)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);
    return db ? db->header.stride / lv_color_format_get_size(db->header.cf) : 0;
}

// ---- quadros desenhados noutra tarefa: enquanto o LVGL manda um quadro ao painel (~45 ms por ecrã inteiro),
// o seguinte desenha-se no buffer de trás, no outro núcleo. Duplo buffer RGB565; o anim.c tem um trinco.
#define ANIM(call) do { anim_lock(); call; anim_unlock(); } while (0)
static QueueHandle_t s_frq;

static void fr_task(void *arg)
{
    fr_t *f;
    for (;;) {
        if (xQueueReceive(s_frq, &f, portMAX_DELAY) != pdTRUE) continue;
        int64_t t0 = esp_timer_get_time();
        anim_lock();
        f->fn(f, (uint32_t *)f->buf[f->front ^ 1], f->stride);
        anim_unlock();
        s_pf_draw += esp_timer_get_time() - t0;
        s_pf_dn++;
        f->state = FR_READY;
    }
}

static bool fr_open(fr_t *f, lv_obj_t *parent, int w, int h, fr_fn_t fn)
{
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < 2; i++) {
        f->buf[i] = heap_caps_calloc(1, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        if (!f->buf[i]) {
            heap_caps_free(f->buf[0]);
            f->buf[0] = NULL;
            return false;
        }
    }
    ANIM(anim_out16(f->buf[0], true); anim_out16(f->buf[1], true));
    f->w = w;
    f->h = h;
    f->fn = fn;
    f->last = -1;
    f->cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(f->cv, f->buf[0], w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(f->cv, LV_OBJ_FLAG_CLICKABLE);
    f->stride = canvas_stride(f->cv);
    return true;
}

// espera pelo quadro em curso e liberta os buffers (o canvas é apagado com o objeto pai)
static void fr_close(fr_t *f)
{
    while (f->state == FR_BUSY) vTaskDelay(1);
    if (f->buf[0]) ANIM(anim_out16(f->buf[0], false); anim_out16(f->buf[1], false));
    heap_caps_free(f->buf[0]);
    heap_caps_free(f->buf[1]);
    f->buf[0] = f->buf[1] = NULL;
    f->cv = NULL;
    f->state = FR_IDLE;
}

// Mostra o quadro pronto. true = pode pedir-se o seguinte (o anterior já está no ecrã) e já passou min_dt.
static bool fr_step(fr_t *f, float now, float min_dt)
{
    if (!f->cv || f->state == FR_BUSY) return false;
    if (f->state == FR_READY) {
        f->front ^= 1;
        lv_canvas_set_buffer(f->cv, f->buf[f->front], f->w, f->h, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(f->cv);
        f->state = FR_IDLE;
    }
    return f->last < 0 || now - f->last >= min_dt;
}

// desenha já no buffer da frente (o 1.º quadro, sem esperar pela tarefa)
static void fr_now(fr_t *f, float t)
{
    f->t = t;
    ANIM(f->fn(f, (uint32_t *)f->buf[f->front], f->stride));
    lv_obj_invalidate(f->cv);
}

static void fr_submit(fr_t *f, float t)
{
    if (f->last < 0) {   // o 1.º quadro desenha-se já, para o ecrã não mostrar o buffer vazio
        fr_now(f, t);
        f->last = t;
        return;
    }
    f->t = f->last = t;
    f->state = FR_BUSY;
    if (xQueueSend(s_frq, &f, 0) != pdTRUE) f->state = FR_IDLE;
}

// Ícone de reserva das boxes (sem imagem do dashd): placa de 8 bits na cor do item com a inicial.
static lv_obj_t *badge_icon(lv_obj_t *parent, const char *label_txt, uint32_t col, int size)
{
    uint32_t *buf = buf_add(size * size * 4);
    lv_obj_t *cv = lv_canvas_create(parent);
    if (!buf) return cv;
    lv_canvas_set_buffer(cv, buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    ANIM(anim_badge(buf, size, canvas_stride(cv), col, label_txt));
    lv_obj_invalidate(cv);
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
EXT_RAM_BSS_ATTR static icon_cache_t s_icache[ICON_CACHE];
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

// Avisos em fila: um de cada vez, com tempo de leitura conforme o tamanho do texto; esperam enquanto houver
// uma notificação aberta. Repetidos são ignorados e "Brilho n%" atualiza o aviso que está à vista.
#define TOAST_Q 6
typedef struct {
    char text[120];
    char level[8];
} toast_q_t;
EXT_RAM_BSS_ATTR static toast_q_t s_tq[TOAST_Q];
static int s_tq_n;
static lv_obj_t *s_toast, *s_toast_l;
static lv_timer_t *s_toast_tm;
static char s_toast_lv[8];

static uint32_t toast_ms(const char *t)
{
    int n = 0;
    for (; *t; t++) n += ((uint8_t)*t & 0xC0) != 0x80;   // caracteres, não bytes
    int ms = 1800 + 65 * n;
    return ms < 2500 ? 2500 : ms > 7000 ? 7000 : ms;
}

static void toast_pump(void);

static void toast_del(lv_timer_t *t)
{
    s_toast_tm = NULL;   // repeat 1: o LVGL apaga o timer
    if (s_toast) lv_obj_delete(s_toast);
    s_toast = s_toast_l = NULL;
    toast_pump();
}

static void toast_make(const char *text, const char *level)
{
    uint32_t c = color_of(level[0] ? level : NULL, C_ACCENT);
    if (!strcmp(level, "info")) c = C_ACCENT;
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
    s_toast = t;
    s_toast_l = l;
    snprintf(s_toast_lv, sizeof(s_toast_lv), "%s", level);
    s_toast_tm = lv_timer_create(toast_del, toast_ms(text), NULL);
    lv_timer_set_repeat_count(s_toast_tm, 1);
}

static void toast_pump(void)
{
    if (s_toast || s_notif || !s_tq_n) return;
    toast_q_t q = s_tq[0];
    memmove(s_tq, s_tq + 1, --s_tq_n * sizeof(s_tq[0]));
    toast_make(q.text, q.level);
}

// uma notificação vai tapar o ecrã: o aviso à vista volta para a frente da fila e reaparece depois
static void toast_hold(void)
{
    if (!s_toast) return;
    if (s_tq_n == TOAST_Q) s_tq_n--;
    memmove(s_tq + 1, s_tq, s_tq_n * sizeof(s_tq[0]));
    snprintf(s_tq[0].text, sizeof(s_tq[0].text), "%s", lv_label_get_text(s_toast_l));
    snprintf(s_tq[0].level, sizeof(s_tq[0].level), "%s", s_toast_lv);
    s_tq_n++;
    if (s_toast_tm) lv_timer_delete(s_toast_tm);
    s_toast_tm = NULL;
    lv_obj_delete(s_toast);
    s_toast = s_toast_l = NULL;
}

static void toast_nolock(const char *text, const char *level)
{
    if (!text || !text[0]) return;
    if (s_toast) {
        const char *cur = lv_label_get_text(s_toast_l);
        bool same = !strcmp(cur, text);
        if (same || (!strncmp(text, "Brilho ", 7) && !strncmp(cur, "Brilho ", 7))) {
            if (!same) lv_label_set_text(s_toast_l, text);
            if (s_toast_tm) lv_timer_reset(s_toast_tm);
            return;
        }
    }
    for (int i = 0; i < s_tq_n; i++)
        if (!strcmp(s_tq[i].text, text)) return;
    if (s_tq_n == TOAST_Q) memmove(s_tq, s_tq + 1, --s_tq_n * sizeof(s_tq[0]));   // fila cheia: sai o mais antigo
    snprintf(s_tq[s_tq_n].text, sizeof(s_tq[0].text), "%s", text);
    snprintf(s_tq[s_tq_n].level, sizeof(s_tq[0].level), "%s", level ? level : "");
    s_tq_n++;
    toast_pump();
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
        bool manual = s_bl_state == 3;
        bl_set(s_bl);
        s_bl_state = 0;
        if (manual) {   // estava a 0% pelo botão: o host fica a saber o brilho de volta (o ciclo segue daqui)
            char b[32];
            snprintf(b, sizeof(b), "{\"t\":\"bl\",\"v\":%d}", s_bl);
            link_send_text(b);
        }
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

// 0% no botão: o ecrã apaga como na inatividade (um toque acorda no brilho anterior, sem abrir nada)
static void bl_off_nolock(void)
{
    bl_set(0);
    s_bl_state = 3;
    if (!s_wake_pane) {
        s_wake_pane = box(lv_layer_top(), 0, 0);
        lv_obj_set_size(s_wake_pane, W, H);
        lv_obj_set_style_bg_opa(s_wake_pane, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_wake_pane, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_pane, wake_pane_cb, LV_EVENT_PRESSED, NULL);
    }
}

bool ui_bl_off(void)
{
    return s_bl_state == 3;
}

static void idle_check(void)
{
    if (s_notif || s_prog || s_pomo || s_bl_state == 3) return;   // o Pomodoro fica sempre aceso enquanto está de lado
    uint32_t idle = lv_display_get_inactive_time(NULL) / 1000;
    if (s_bl_state == 0 && s_dim_s > 0 && idle >= (uint32_t)s_dim_s) {
        bl_set(s_bl > 10 ? 10 : s_bl);
        s_bl_state = 1;
        if (!s_wake_pane) {
            s_wake_pane = box(lv_layer_top(), 0, 0);
            lv_obj_set_size(s_wake_pane, W, H);
            lv_obj_set_style_bg_opa(s_wake_pane, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(s_wake_pane, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_wake_pane, wake_pane_cb, LV_EVENT_PRESSED, NULL);
        }
    } else if (s_bl_state == 1 && s_off_s > 0 && idle >= (uint32_t)s_off_s) {
        bl_set(0);
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
    int32_t avail = W - 44 - 132 - (back ? 52 : 0);   // painel menos os cantos, o estado à direita e o "voltar"
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
    fr_close(&s_boot_fr);
    if (s_boot_cv) lv_obj_delete(s_boot_cv);
    s_boot_cv = NULL;
}

static void boot_draw_fn(fr_t *f, uint32_t *buf, int stride)
{
    if (f->a.boot.away) anim_away_draw(buf, f->w, f->h, stride, f->t, &f->a.boot.a);
    else anim_boot_draw(buf, stride, f->t, f->a.boot.prog, f->a.boot.st, f->a.boot.tilt);
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
    fr_t *f = &s_boot_fr;
    if (!fr_step(f, sec, 0.2f)) return;
    f->a.boot.away = true;
    strlcpy(f->a.boot.hm, hm, sizeof(f->a.boot.hm));
    strlcpy(f->a.boot.date, date, sizeof(f->a.boot.date));
    strlcpy(f->a.boot.st, st, sizeof(f->a.boot.st));
    f->a.boot.a = (away_t){.time = f->a.boot.hm, .date = f->a.boot.date, .bat = p.percent, .chg = p.charging,
                           .pending = sd_events_pending(), .sd = sd_ok(), .status = f->a.boot.st, .shift = tm.tm_min % 9};
    fr_submit(f, sec);
}

static void boot_anim(lv_timer_t *t)
{
    if (!s_boot_cv) return;
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
    fr_t *f = &s_boot_fr;
    if (!fr_step(f, sec, 0)) return;
    f->a.boot.away = false;
    f->a.boot.st[0] = '\0';
    if (!s_boot_exit) boot_status(f->a.boot.st, sizeof(f->a.boot.st));
    f->a.boot.prog = s_boot_prog;
    f->a.boot.tilt = imu_tilt();
    fr_submit(f, sec);
}

static void boot_show_mode(bool away)
{
    if (s_boot_cv) return;
    body_free();
    s_id[0] = '\0';
    s_page = -1;
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    set_title("");
    set_dots(0, 0);
    if (!fr_open(&s_boot_fr, s_scr, BOOT_W, BOOT_H, boot_draw_fn)) return;
    s_boot_cv = s_boot_fr.cv;
    lv_obj_set_pos(s_boot_cv, 0, HDR_H);
    s_boot_min = s_boot_first;
    s_boot_first = false;
    s_boot_exit = false;
    s_boot_exit_t = 0;
    s_boot_prog = 0;
    s_away = away;
    if (s_bgimg) lv_obj_add_flag(s_bgimg, LV_OBJ_FLAG_HIDDEN);
    ANIM(anim_boot_start(BOOT_W, BOOT_H));
    s_boot_t0 = esp_timer_get_time();
    s_boot_tm = lv_timer_create(boot_anim, 15, NULL);
    boot_anim(NULL);
}

static void boot_show(void)
{
    boot_show_mode(false);
}

// ---- entrada das telas
static void intro_start(const char *tpl, int slide_dir)
{
    // as boxes (menus) entram sem animação: deslizar as imagens todas pesava demasiado
    if (tpl && !strcmp(tpl, "menu")) return;
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
    if (!slide_dir) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_body);
        lv_anim_set_exec_cb(&a, anim_ty_cb);
        lv_anim_set_values(&a, 36, 0);
        lv_anim_set_duration(&a, 240);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

// ------------------------------------------------------------------------------------- templates

// cores das placas de reserva quando o item não diz a sua (ciano, violeta, rosa, laranja, verde, âmbar)
static const uint32_t PAL_BADGE[6] = {0x2DB8E0, 0x7C6BFF, 0xE8508F, 0xF07A3A, 0x3FBF6A, 0xE0A020};

// seta da variação (Stocks: "mark" up/down) num canvas pequeno: triângulo cheio
static void mark_arrow(lv_obj_t *parent, bool up, uint32_t col)
{
    enum { MW = 22, MH = 16 };
    uint32_t *px = buf_add(MW * MH * 4);
    if (!px) return;
    for (int y = 0; y < MH; y++) {
        int r = up ? y : MH - 1 - y;          // linha a contar da ponta
        int half = (r * (MW / 2)) / (MH - 1);
        for (int x = 0; x < MW; x++)
            px[y * MW + x] = (x >= MW / 2 - 1 - half && x <= MW / 2 + half) ? (0xFF000000u | col) : 0;
    }
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, px, MW, MH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_align(cv, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
}

static void build_menu(const cJSON *scr)
{
    // "icon_size": ícones mais pequenos numa tela (ex.: logotipos das Stocks); centrados no mesmo lugar
    int isz = jint(scr, "icon_size", 84);
    if (isz < 24 || isz > 84) isz = 84;
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
        // ícone: a imagem do dashd (icons/<id>.bin) ou uma posta no cartão; sem ela, uma placa de 8 bits
        uint32_t tint = color_of(jstr(it, "color"), PAL_BADGE[lv_obj_get_child_count(s_body) % 6]);
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
            lv_obj_set_size(cv, isz, isz);
            lv_image_set_inner_align(cv, LV_IMAGE_ALIGN_CONTAIN);
        } else {
            cv = badge_icon(t, jstr(it, "label"), tint, isz);
        }
        lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 8 + (84 - isz) / 2);
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
        // selo vermelho (ex.: mensagens não lidas): o dashd manda-o em texto ("3"), como ao DashDeck
        const cJSON *bj = cJSON_GetObjectItem(it, "badge");
        char btxt[8] = "";
        if (cJSON_IsNumber(bj) && bj->valueint > 0) snprintf(btxt, sizeof(btxt), "%d", bj->valueint > 99 ? 99 : bj->valueint);
        else if (cJSON_IsString(bj) && bj->valuestring[0] && strcmp(bj->valuestring, "0") != 0) {
            int n = atoi(bj->valuestring);
            if (n > 99) strcpy(btxt, "99+");
            else strlcpy(btxt, bj->valuestring, sizeof(btxt));
        }
        if (btxt[0]) {
            lv_obj_t *b = box(t, C_ERR, LV_RADIUS_CIRCLE);
            lv_obj_set_style_border_width(b, 3, 0);               // aro escuro: destaca-se do ícone
            lv_obj_set_style_border_color(b, lv_color_hex(0x0D1117), 0);
            lv_obj_set_height(b, 36);
            lv_obj_set_style_min_width(b, 36, 0);
            lv_obj_set_width(b, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(b, 8, 0);
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -4, 4);
            lv_obj_t *bl = label(b, btxt, &deck_font_16, 0xFFFFFF);
            lv_obj_center(bl);
        }
        const char *mark = jstr(it, "mark");
        if (mark && (!strcmp(mark, "up") || !strcmp(mark, "down")))
            mark_arrow(t, mark[0] == 'u', color_of(jstr(it, "mark_color"), mark[0] == 'u' ? C_OK : C_ERR));
        const char *dot = jstr(it, "dot");
        if (dot && *dot) {
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
    bool beside = rest >= 130 && cJSON_GetArraySize(cJSON_GetObjectItem(scr, "rows")) > 0;
    lv_obj_set_flex_flow(s_body, beside ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_body, 10, 0);
    plot_create(s_body, p, &env);
    if (beside) build_kv_in(s_body, scr, rest);
    else build_kv_in(s_body, scr, lv_pct(100));
}


// ---- "input": escrever texto com o teclado (Meshtastic). "to"/"to_sub": card do destino (toque: tap "dest");
// "placeholder", "max" (caracteres), "text". Enviar (✓) manda {"t":"input","id","text"}. O texto escrito fica
// num rascunho da tela: sobrevive a um update, ao seletor do destino e ao voltar.
#define INPUT_MAX 240
static lv_obj_t *s_in_ta, *s_in_cnt, *s_in_kb;
static char s_draft_id[48];
EXT_RAM_BSS_ATTR static char s_draft[INPUT_MAX * 4 + 1];   // UTF-8
static int s_in_max = 200;

#define KB_ACC "áé"
static const char *const KB_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "ç", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", KB_ACC, ",", " ", ".", LV_SYMBOL_OK, ""};
static const char *const KB_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "Ç", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", KB_ACC, ",", " ", ".", LV_SYMBOL_OK, ""};
static const char *const KB_SPECIAL[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "€", "%", "&", "-", "+", "(", ")", "/", "\n",
    "abc", "!", "?", ":", ";", "'", "\"", "*", LV_SYMBOL_BACKSPACE, "\n",
    "abc", KB_ACC, ",", " ", ".", LV_SYMBOL_OK, ""};
static const char *const KB_ACCENT[] = {
    "á", "à", "â", "ã", "é", "ê", "í", "ó", "ô", "õ", "\n",
    "ú", "ü", "Á", "À", "Â", "Ã", "É", "Ê", "Í", "Ó", "\n",
    "abc", "Ô", "Õ", "Ú", "ª", "º", "¿", "¡", LV_SYMBOL_BACKSPACE, "\n",
    "1#", "abc", ",", " ", ".", LV_SYMBOL_OK, ""};
#define KB_K  (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG)
#define KB_M  (KB_K | LV_BUTTONMATRIX_CTRL_CHECKED | 3)   // teclas de modo (mais largas e a outra cor)
#define KB_M5 (KB_K | LV_BUTTONMATRIX_CTRL_CHECKED | 5)   // "ABC" e ⌫ na 3.ª fila (cabem o texto)
#define KB_BS (LV_BUTTONMATRIX_CTRL_CHECKED | 5)
static const lv_buttonmatrix_ctrl_t KB_CTRL_ABC[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KB_M5, 4, 4, 4, 4, 4, 4, 4, KB_BS,
    KB_M, KB_M, 2, 6 | KB_K, 2, KB_M};
static const lv_buttonmatrix_ctrl_t KB_CTRL_SPEC[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    KB_M5, 4, 4, 4, 4, 4, 4, 4, KB_BS,
    KB_M, KB_M, 2, 6 | KB_K, 2, KB_M};

static void input_count(void)
{
    if (!s_in_ta || !s_in_cnt) return;
    const char *t = lv_textarea_get_text(s_in_ta);
    lv_label_set_text_fmt(s_in_cnt, "%d/%d", (int)lv_text_get_encoded_length(t), s_in_max);
}

static void input_changed(lv_event_t *e)
{
    (void)e;
    if (!s_in_ta) return;
    strlcpy(s_draft, lv_textarea_get_text(s_in_ta), sizeof(s_draft));
    strlcpy(s_draft_id, s_id, sizeof(s_draft_id));
    input_count();
}

static void input_kb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t b = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = b == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, b);
    if (txt && !strcmp(txt, KB_ACC)) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
        return;
    }
    if (txt && !strcmp(txt, LV_SYMBOL_OK)) {
        const char *t = s_in_ta ? lv_textarea_get_text(s_in_ta) : "";
        if (!t[0]) return;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "input");
        cJSON_AddStringToObject(o, "id", s_id);
        cJSON_AddStringToObject(o, "text", t);
        link_send(o);
        cJSON_Delete(o);
        s_draft[0] = '\0';   // enviado: a próxima mensagem começa vazia (o texto fica à vista até o dashd responder)
        s_draft_id[0] = '\0';
        return;
    }
    lv_keyboard_def_event_cb(e);
    if (lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER && txt && strlen(txt) == 1 && txt[0] >= 'A' && txt[0] <= 'Z')
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);   // maiúscula só para uma letra (como nos telemóveis)
}

static void build_input(const cJSON *scr)
{
    lv_obj_set_style_pad_gap(s_body, 8, 0);
    lv_obj_set_style_pad_bottom(s_body, 6, 0);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    const cJSON *jm = cJSON_GetObjectItem(scr, "max");
    s_in_max = cJSON_IsNumber(jm) && jm->valueint > 0 && jm->valueint <= INPUT_MAX ? jm->valueint : 200;
    if (jstr(scr, "to")) {   // destino: toque para mudar
        lv_obj_t *c = box(s_body, C_CARD, 16);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x172638), 0);
        lv_obj_set_size(c, lv_pct(100), 52);
        lv_obj_set_style_pad_hor(c, 14, 0);
        make_clickable(c, 'i', "dest", NULL);
        lv_obj_t *l = label(c, jstr(scr, "to"), &deck_font_20, C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 300);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, jstr(scr, "to_sub") ? -9 : 0);
        if (jstr(scr, "to_sub")) {
            lv_obj_t *s2 = label(c, jstr(scr, "to_sub"), &deck_font_16, C_SUB);
            lv_label_set_long_mode(s2, LV_LABEL_LONG_DOT);
            lv_obj_set_width(s2, 300);
            lv_obj_align(s2, LV_ALIGN_LEFT_MID, 0, 12);
        }
        lv_obj_t *m = label(c, "Mudar", &deck_font_20, 0x58A6FF);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_t *wrap = box(s_body, C_CARD, 16);
    lv_obj_set_size(wrap, lv_pct(100), 88);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    s_in_ta = lv_textarea_create(wrap);
    lv_obj_set_size(s_in_ta, lv_pct(100), 88);
    lv_obj_set_style_bg_opa(s_in_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_in_ta, 0, 0);
    lv_obj_set_style_pad_all(s_in_ta, 10, 0);
    lv_obj_set_style_pad_bottom(s_in_ta, 22, 0);
    lv_obj_set_style_text_font(s_in_ta, &deck_font_20, 0);
    lv_obj_set_style_text_color(s_in_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(s_in_ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_in_ta, lv_color_hex(C_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_border_color(s_in_ta, lv_color_hex(C_ACCENT), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_max_length(s_in_ta, s_in_max);
    lv_textarea_set_placeholder_text(s_in_ta, jstr(scr, "placeholder") ? jstr(scr, "placeholder") : "Mensagem");
    const char *init = !strcmp(s_draft_id, s_id) ? s_draft : (jstr(scr, "text") ? jstr(scr, "text") : "");
    lv_textarea_set_text(s_in_ta, init);
    lv_obj_add_state(s_in_ta, LV_STATE_FOCUSED);   // cursor à vista
    lv_obj_add_event_cb(s_in_ta, input_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_in_cnt = label(wrap, "", &deck_font_16, C_SUB);
    lv_obj_align(s_in_cnt, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
    input_count();

    s_in_kb = lv_keyboard_create(s_body);
    lv_obj_set_size(s_in_kb, lv_pct(100), LV_PCT(100));
    lv_obj_set_flex_grow(s_in_kb, 1);
    lv_keyboard_set_map(s_in_kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char **)KB_LOWER, KB_CTRL_ABC);
    lv_keyboard_set_map(s_in_kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char **)KB_UPPER, KB_CTRL_ABC);
    lv_keyboard_set_map(s_in_kb, LV_KEYBOARD_MODE_SPECIAL, (const char **)KB_SPECIAL, KB_CTRL_SPEC);
    lv_keyboard_set_map(s_in_kb, LV_KEYBOARD_MODE_USER_1, (const char **)KB_ACCENT, KB_CTRL_SPEC);
    lv_keyboard_set_mode(s_in_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(s_in_kb, false);
    lv_keyboard_set_textarea(s_in_kb, s_in_ta);
    lv_obj_remove_event_cb(s_in_kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_in_kb, input_kb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_in_kb, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(s_in_kb, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_in_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_in_kb, 0, 0);
    lv_obj_set_style_pad_all(s_in_kb, 0, 0);
    lv_obj_set_style_pad_gap(s_in_kb, 5, 0);
    lv_obj_set_style_bg_color(s_in_kb, lv_color_hex(0x1F2833), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_in_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_in_kb, lv_color_hex(0x2A3442), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_in_kb, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_in_kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_in_kb, lv_color_hex(C_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_in_kb, &deck_font_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_in_kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_in_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_in_kb, 0, LV_PART_ITEMS);
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
        if (!strcmp(c->live, "bl")) bl_set(v < 2 ? 2 : v);
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

static void build_body(const cJSON *scr)
{
    body_free();
    s_in_ta = s_in_cnt = s_in_kb = NULL;
    lv_obj_set_style_pad_bottom(s_body, 28, 0);   // o "input" ocupa o fundo todo
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_body, 12, 0);
    const char *tpl = jstr(scr, "tpl");
    if (!tpl) tpl = "text";
    build_sliders(scr);   // extras de qualquer template: sliders e gráficos antes do corpo
    build_charts(scr);
    if (strcmp(tpl, "clock") != 0) clock_hide();
    if (strcmp(tpl, "plot") != 0) plot_release();   // o buffer da câmara só vive nas telas "plot"
    if (!strcmp(tpl, "panel")) { /* só os extras */ }
    else if (!strcmp(tpl, "clock")) clock_apply(scr);
    else if (!strcmp(tpl, "menu")) build_menu(scr);
    else if (!strcmp(tpl, "image")) build_image(scr);
    else if (!strcmp(tpl, "plot")) build_plot(scr);
    else if (!strcmp(tpl, "input")) build_input(scr);
    else if (!strcmp(tpl, "log")) build_log(scr);
    else if (!strcmp(tpl, "list")) build_list(scr);
    else if (!strcmp(tpl, "kv")) build_kv(scr);
    else if (!strcmp(tpl, "text")) build_text(scr);
    else {
        char msg[80];
        snprintf(msg, sizeof(msg), "Template \"%s\" ainda não existe no DashDeck³", tpl);
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
    plot_live_forget();   // tela nova (ou outra página): a imagem antiga da câmara não fica
    cJSON_Delete(s_screen);
    s_screen = cJSON_Duplicate(msg, true);
    if (s_boot_cv) {   // ecrã de arranque: a tela entra quando a sequência acabar
        s_boot_exit = true;
        return;
    }
    apply_screen(true);
}

static void update_screen(const cJSON *msg)
{
    const char *id = jstr(msg, "id");
    if (!s_screen || !id) return;
    if (s_boot_cv) {   // ainda no arranque: junta à tela pendente
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
    if (s_boot_cv) return;
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

// Notificações em fila: a seguinte só aparece quando a atual fecha (tempo, toque ou voltar)
#define NOTIF_Q 4
static cJSON *s_nq[NOTIF_Q];
static int s_nq_n;
static void notify_start(const cJSON *msg);

static void notif_free(void)
{
    if (s_notif_local) s_ring_until = 0;
    s_notif_local = false;
    if (s_notif_tm) lv_timer_delete(s_notif_tm);
    s_notif_tm = NULL;
    if (s_notif) lv_obj_delete(s_notif);
    s_notif = s_notif_cv = s_notif_band = NULL;
    heap_caps_free(s_notif_buf);
    s_notif_buf = NULL;
}

static void notif_close(void)
{
    notif_free();
    if (s_nq_n) {
        cJSON *m = s_nq[0];
        memmove(s_nq, s_nq + 1, --s_nq_n * sizeof(s_nq[0]));
        notify_start(m);
        cJSON_Delete(m);
    } else {
        toast_pump();
    }
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
    ANIM(anim_notify_draw(s_notif_buf, NCV_W, NCV_H, canvas_stride(s_notif_cv), el));
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
    if (!s_notif_local) link_send_event("notify", "act", "open", NULL, NULL);
    notif_close();
}

static void notify_show(const cJSON *msg)
{
    // varanda aberta: as mensagens (não os alarmes nem as emergências) chegam com o pombo
    if (s_var && !jstr(msg, "kind") && !cJSON_IsTrue(cJSON_GetObjectItem(msg, "local"))) {
        varanda_event(VEV_DM, 0, 0, jstr(msg, "from") ? jstr(msg, "from") : jstr(msg, "title"), jstr(msg, "text"));
        if (!cJSON_IsFalse(cJSON_GetObjectItem(msg, "sound"))) { audio_tone(1319, 70, 0); audio_tone(1760, 90, 0); }
        return;
    }
    if (!s_notif) {
        notify_start(msg);
        return;
    }
    if (s_nq_n == NOTIF_Q) {   // fila cheia: sai a mais antiga
        cJSON_Delete(s_nq[0]);
        memmove(s_nq, s_nq + 1, --s_nq_n * sizeof(s_nq[0]));
    }
    s_nq[s_nq_n++] = cJSON_Duplicate(msg, 1);
}

static void notify_start(const cJSON *msg)
{
    notif_free();
    toast_hold();
    wake_nolock();
    const char *k = jstr(msg, "kind");
    int kind = k && !strcmp(k, "alarm") ? 1 : k && !strcmp(k, "squawk") ? 2 : 0;
    int spr = anim_sprite_id(jstr(msg, "sprite"));   // "mail" "clock" "plane"; sem ele, o do tipo
    if (spr < 0) spr = kind == 1 ? SPR_CLOCK : kind == 2 ? SPR_PLANE : SPR_MAIL;
    s_notif_buf = heap_caps_calloc(1, NCV_W * NCV_H * 4, MALLOC_CAP_SPIRAM);
    if (!s_notif_buf) return;
    const char *from = jstr(msg, "from");
    ANIM(anim_notify_start(spr, from ? from : jstr(msg, "title"), kind));

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

    // sem "dur": animação + tempo de leitura do texto (6 a 20 s)
    const char *txt = jstr(msg, "text");
    int n = 0;
    for (const char *c = txt ? txt : ""; *c; c++) n += ((uint8_t)*c & 0xC0) != 0x80;
    float rd = NOTIFY_ANIM + 2.5f + n * 0.07f;
    rd = rd < 6 ? 6 : rd > 20 ? 20 : rd;
    int dur = jint(msg, "dur", 0);
    s_notif_total = dur > 0 && dur <= 300 ? (float)dur : rd;
    s_notif_local = cJSON_IsTrue(cJSON_GetObjectItem(msg, "local"));   // do próprio cubo: o toque não vai ao host
    if (s_notif_local && cJSON_IsTrue(cJSON_GetObjectItem(msg, "ring")))   // alarme: toca até ao fim ou a um toque
        s_ring_until = esp_timer_get_time() + (int64_t)(s_notif_total * 1e6f);
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
// previsão, a paisagem e o cão). Swipe para os lados troca a face (guardada no cubo); um toque volta, menos
// nas zonas da face do temporizador e alarme.
// O temporizador e o alarme são só do cubo: guardados na NVS, contados aqui (tick_1s, mesmo sem uConsole)
// e tocados na coluna do cubo. Os do DashDeck (C6 + som no uConsole) são outros.
static lv_obj_t *s_clk, *s_clk_name;
static fr_t s_clk_fr;
static lv_timer_t *s_clk_tm;
static int64_t s_clk_t0, s_clk_name_until;
static int s_clk_face;
EXT_RAM_BSS_ATTR static clock3_t s_clk_data;
static char s_clk_bg[48];

static void clock_hide(void)
{
    if (s_clk_tm) lv_timer_delete(s_clk_tm);
    s_clk_tm = NULL;
    fr_close(&s_clk_fr);
    if (s_clk) lv_obj_delete(s_clk);
    s_clk = s_clk_name = NULL;
}

// epoch "local" (a hora local contada como se fosse UTC; é a base de tend): 0 sem hora acertada
static int64_t local_now(struct tm *out)
{
    time_t now = time(NULL);
    struct tm tm, g;
    localtime_r(&now, &tm);
    if (out) *out = tm;
    if (tm.tm_year <= 120) return 0;
    gmtime_r(&now, &g);
    int dd = tm.tm_yday - g.tm_yday;
    if (dd > 1) dd = -1;   // passagem de ano
    if (dd < -1) dd = 1;
    return (int64_t)now + dd * 86400 + (tm.tm_hour - g.tm_hour) * 3600 + (tm.tm_min - g.tm_min) * 60;
}

static void clock_load(void)
{
    clock3_t *c = &s_clk_data;
    c->tset = store_get("tset", 300);
    c->tend = store_get("tend", 0);
    c->tleft = store_get("tleft", 0);
    int hm = store_get("alhm", 7 * 60 + 30);
    c->alh = (hm / 60) % 24;
    c->alm = hm % 60;
    c->alon = store_get("alon", 0) != 0;
}

static void clock_save(void)
{
    clock3_t *c = &s_clk_data;
    store_set("tset", c->tset);
    store_set("tend", (int32_t)c->tend);
    store_set("tleft", c->tleft);
    store_set("alhm", c->alh * 60 + c->alm);
    store_set("alon", c->alon);
}

static void clock_ring(bool alarm)
{
    clock3_t *c = &s_clk_data;
    char from[24], title[40], text[48];
    if (alarm) {
        snprintf(from, sizeof(from), "ALARME %02d:%02d", c->alh, c->alm);
        snprintf(title, sizeof(title), "Alarme das %02d:%02d", c->alh, c->alm);
        snprintf(text, sizeof(text), "Toque para parar");
    } else {
        snprintf(from, sizeof(from), "TEMPO!");
        snprintf(title, sizeof(title), "Temporizador terminado");
        int mn = c->tset / 60, sc = c->tset % 60;
        if (mn && sc) snprintf(text, sizeof(text), "%d min %d s · toque para parar", mn, sc);
        else if (mn) snprintf(text, sizeof(text), "%d min · toque para parar", mn);
        else snprintf(text, sizeof(text), "%d s · toque para parar", sc);
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "kind", "alarm");
    cJSON_AddStringToObject(m, "from", from);
    cJSON_AddStringToObject(m, "title", title);
    cJSON_AddStringToObject(m, "text", text);
    cJSON_AddNumberToObject(m, "dur", 60);
    cJSON_AddBoolToObject(m, "sound", false);   // o som é o toque contínuo de clock_check
    cJSON_AddBoolToObject(m, "local", true);
    cJSON_AddBoolToObject(m, "ring", true);
    notify_show(m);
    cJSON_Delete(m);
}

// a cada segundo (tick_1s): fim do temporizador, hora do alarme e o bip-bip enquanto toca
static void clock_check(void)
{
    static int32_t last_al = -1;   // minuto (dia do ano * 1440 + minuto) do último alarme
    clock3_t *c = &s_clk_data;
    struct tm tm;
    int64_t now = local_now(&tm);
    if (now && c->tend && now >= c->tend) {
        c->tend = 0;
        c->tleft = 0;
        clock_save();
        clock_ring(false);
    }
    int32_t stamp = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    if (now && c->alon && tm.tm_hour == c->alh && tm.tm_min == c->alm && stamp != last_al) {
        last_al = stamp;
        clock_ring(true);
    }
    if (s_ring_until && esp_timer_get_time() < s_ring_until && (esp_timer_get_time() / 1000000) % 2 == 0) {
        audio_tone(988, 120, 0);
        audio_tone(0, 80, 0);
        audio_tone(988, 120, 0);
        audio_tone(0, 80, 0);
        audio_tone(1319, 160, 0);
    } else if (s_ring_until && esp_timer_get_time() >= s_ring_until) {
        s_ring_until = 0;
    }
}

static void clk_draw_fn(fr_t *f, uint32_t *buf, int stride)
{
    anim_clock_draw(f->a.clk.face, buf, f->w, f->h, stride, f->t, &f->a.clk.c);
}

static void clock_tick(lv_timer_t *t)
{
    float el = (esp_timer_get_time() - s_clk_t0) / 1e6f;
    if (s_clk_name && esp_timer_get_time() > s_clk_name_until) lv_obj_add_flag(s_clk_name, LV_OBJ_FLAG_HIDDEN);
    if (!fr_step(&s_clk_fr, el, 0.04f)) return;   // até 25 qps
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
    clock3_t *c = &s_clk_data;
    c->time = hm;
    c->date = date;
    c->bat = p.percent;
    c->chg = p.charging;
    c->shift = (int)((now / 60) % 9);
    c->now = local_now(NULL);
    c->scene = bg_get(s_clk_bg);
    fr_t *f = &s_clk_fr;
    f->a.clk.c = *c;
    strlcpy(f->a.clk.hm, hm, sizeof(f->a.clk.hm));
    strlcpy(f->a.clk.date, date, sizeof(f->a.clk.date));
    f->a.clk.c.time = f->a.clk.hm;
    f->a.clk.c.date = f->a.clk.date;
    f->a.clk.face = s_clk_face;
    c->time = c->date = "";   // hm e date são locais: não deixar ponteiros para a pilha
    fr_submit(f, el);
}

static void clock_face_name(void)
{
    lv_label_set_text(s_clk_name, anim_clock_name(s_clk_face));
    lv_obj_remove_flag(s_clk_name, LV_OBJ_FLAG_HIDDEN);
    s_clk_name_until = esp_timer_get_time() + 1500000;
}

// Toque numa zona do temporizador/alarme: muda e guarda no cubo (o host não sabe nem precisa de saber).
static void clock_zone(int z)
{
    clock3_t *c = &s_clk_data;
    bool run = c->tend > 0, paused = !run && c->tleft > 0;
    c->now = local_now(NULL);
    switch (z) {
    case CZ_TMIN_UP: case CZ_TMIN_DN: case CZ_TSEC_UP: case CZ_TSEC_DN:
        if (run || paused) return;
        c->tset += z == CZ_TMIN_UP ? 60 : z == CZ_TMIN_DN ? -60 : z == CZ_TSEC_UP ? 10 : -10;
        if (c->tset < 0) c->tset = 0;
        if (c->tset > 99 * 60 + 59) c->tset = 99 * 60 + 59;
        break;
    case CZ_TGO:
        if (run) {
            c->tleft = (int)(c->tend - c->now);
            c->tend = 0;
        } else {
            int left = paused ? c->tleft : c->tset;
            if (left <= 0 || !c->now) return;   // sem hora acertada não dá para contar
            c->tend = c->now + left;
            c->tleft = 0;
        }
        break;
    case CZ_TRESET:
        c->tend = 0;
        c->tleft = 0;
        break;
    case CZ_AH_UP: case CZ_AH_DN: case CZ_AM_UP: case CZ_AM_DN:
        if (z == CZ_AH_UP) c->alh = (c->alh + 1) % 24;
        if (z == CZ_AH_DN) c->alh = (c->alh + 23) % 24;
        if (z == CZ_AM_UP) c->alm = (c->alm + 1) % 60;
        if (z == CZ_AM_DN) c->alm = (c->alm + 59) % 60;
        break;
    case CZ_AON:
        c->alon = !c->alon;
        break;
    }
    clock_save();
}

static int clock_zone_at(void)
{
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    return anim_clock_zone(s_clk_face, pt.x, pt.y);
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
    } else if (code == LV_EVENT_LONG_PRESSED_REPEAT) {   // manter o dedo numa seta repete
        int z = clock_zone_at();
        if (z != CZ_NONE && z != CZ_TGO && z != CZ_TRESET && z != CZ_AON) clock_zone(z);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        int z = clock_zone_at();
        if (z != CZ_NONE) clock_zone(z);
        else send_tap("back", NULL, NULL);
    }
}

// Dados do "clock" do dashd: previsão, ícone, fundo da paisagem, cão e chuva/neve.
static void clock_apply(const cJSON *scr)
{
    const cJSON *c = cJSON_GetObjectItem(scr, "clock");
    clock3_t *d = &s_clk_data;
    const cJSON *wx = cJSON_GetObjectItem(c, "wx");
    d->has_wx = cJSON_IsObject(wx);
    if (d->has_wx) {
        d->temp = jint(wx, "t", 0);
        d->tmax = jint(wx, "max", 0);
        d->tmin = jint(wx, "min", 0);
        d->rain = jint(wx, "rain", 0);
    }
    const cJSON *ic = cJSON_GetObjectItem(c, "icon");
    const char *dd = jstr(ic, "d");
    const cJSON *pal = cJSON_GetObjectItem(ic, "pal");
    d->has_icon = dd && strlen(dd) >= 256 && cJSON_IsArray(pal);
    if (d->has_icon) {
        for (int i = 0; i < 256; i++) d->icon[i] = (uint8_t)(dd[i] - '0');
        for (int i = 0; i < 8; i++) d->icon_pal[i] = color_of(cJSON_GetStringValue(cJSON_GetArrayItem(pal, i)), 0);
    }
    strlcpy(s_clk_bg, jstr(c, "bg") ? jstr(c, "bg") : "", sizeof(s_clk_bg));
    const char *an = jstr(c, "anim");
    d->anim = !an ? CLK_NONE : !strcmp(an, "rain") ? CLK_RAIN : !strcmp(an, "storm") ? CLK_STORM
            : !strcmp(an, "snow") ? CLK_SNOW : CLK_NONE;
    d->night = cJSON_IsTrue(cJSON_GetObjectItem(c, "night"));
    const cJSON *sk = cJSON_GetObjectItem(c, "sky");   // céu real: luz, crepúsculo, nuvens, sol, lua
    d->sky.ok = cJSON_IsObject(sk);
    if (d->sky.ok) {
        d->sky.l = jint(sk, "l", 0);
        d->sky.g = jint(sk, "g", 0);
        d->sky.cf = jint(sk, "cf", 0);
        d->sky.sun = jint(sk, "sun", -1);
        d->sky.moon = jint(sk, "moon", -1);
        d->sky.ph = jint(sk, "ph", 50);
    }
    const cJSON *pj = cJSON_GetObjectItem(c, "pet");   // [x da casa, base, luz 0..100, 1 brinca|0 dorme]
    if (cJSON_GetArraySize(pj) >= 4) {
        d->pet_x = cJSON_GetArrayItem(pj, 0)->valueint;
        d->pet_y = cJSON_GetArrayItem(pj, 1)->valueint;
        d->pet_l = cJSON_GetArrayItem(pj, 2)->valueint;
        d->pet = cJSON_GetArrayItem(pj, 3)->valueint ? 1 : 0;
    } else {
        d->pet = -1;
    }
    if (s_clk) return;
    s_clk_face = (int)store_get("clkface", 0) % ANIM_CLOCK_FACES;
    s_clk = box(s_scr, 0x000000, 0);   // por cima do cabeçalho e do corpo; os avisos ficam por cima (camada de topo)
    lv_obj_set_size(s_clk, W, H);
    lv_obj_add_flag(s_clk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_clk, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_clk, clock_ev, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_clk, clock_ev, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_clk, clock_ev, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    if (!fr_open(&s_clk_fr, s_clk, W, H, clk_draw_fn)) {
        lv_obj_delete(s_clk);
        s_clk = NULL;
        return;
    }
    s_clk_name = label(s_clk, "", &deck_font_20, 0xFFFFFF);
    lv_obj_set_style_bg_color(s_clk_name, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_clk_name, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_clk_name, 14, 0);
    lv_obj_set_style_pad_ver(s_clk_name, 4, 0);
    lv_obj_set_style_radius(s_clk_name, 14, 0);
    lv_obj_align(s_clk_name, LV_ALIGN_BOTTOM_MID, 0, -14);
    clock_face_name();
    s_clk_t0 = esp_timer_get_time();
    s_clk_tm = lv_timer_create(clock_tick, 15, NULL);
    clock_tick(NULL);
}

// ------------------------------------------------------------------------------------- Pomodoro
// O cubo em pé na mesa e girado 90° para a direita (imu.c) abre o Pomodoro; de volta ao normal, fecha.
// 25 min de foco, 5 de pausa e, a cada 4 focos, 15 de pausa longa. A contagem continua com o ecrã
// fechado (pomo_check no tick_1s); no fim de cada fase o cubo toca e, se o ecrã estiver fechado, avisa.
// O ecrã é desenhado direito para quem olha (anim_pomo_draw) e rodado 90° para o painel.
static const int POMO_LEN[] = {25 * 60, 5 * 60, 15 * 60};
static struct {
    int phase, done, today, yday, left;
    bool run;
    int64_t end_us;
} s_po = {.left = 25 * 60, .yday = -1};
static lv_obj_t *s_pomo_cv;
static uint16_t *s_pomo_buf, *s_pomo_up;
static lv_timer_t *s_pomo_tm;
static int64_t s_pomo_t0;

static int pomo_left(void)
{
    if (!s_po.run) return s_po.left;
    int64_t us = s_po.end_us - esp_timer_get_time();
    return us <= 0 ? 0 : (int)((us + 999999) / 1000000);
}

static void pomo_count(void)   // mais um foco concluído hoje (guardado; recomeça a zero noutro dia)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int yd = tm.tm_year > 120 ? tm.tm_yday : s_po.yday;
    if (yd != s_po.yday) s_po.today = 0;
    s_po.yday = yd;
    s_po.today++;
    store_set("po_today", s_po.today);
    store_set("po_yday", s_po.yday);
}

// passa à fase seguinte; natural = o tempo acabou (conta o foco e a pausa começa sozinha)
static void pomo_next(bool natural)
{
    if (s_po.phase == POMO_FOCUS) {
        if (natural) {
            s_po.done++;
            pomo_count();
        }
        s_po.phase = s_po.done >= 4 ? POMO_LONG : POMO_SHORT;
        s_po.run = natural;
    } else {
        if (s_po.phase == POMO_LONG) s_po.done = 0;
        s_po.phase = POMO_FOCUS;
        s_po.run = false;   // o foco seguinte começa com um toque
    }
    s_po.left = POMO_LEN[s_po.phase];
    if (s_po.run) s_po.end_us = esp_timer_get_time() + (int64_t)s_po.left * 1000000;
}

static void pomo_draw(void)
{
    if (!s_pomo) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (tm.tm_year > 120 && tm.tm_yday != s_po.yday && s_po.yday >= 0) s_po.today = 0;   // outro dia
    pomo_t p = {.phase = s_po.phase, .total = POMO_LEN[s_po.phase], .left = pomo_left(), .run = s_po.run,
                .done = s_po.done, .today = s_po.today, .clock = tm.tm_year > 120 ? hm : NULL};
    ANIM(anim_pomo_draw((uint32_t *)s_pomo_up, W, H, W, (esp_timer_get_time() - s_pomo_t0) / 1e6f, &p));
    // rodar 90°: o topo de quem olha é o lado esquerdo do painel (cubo girado para a direita)
    int st = canvas_stride(s_pomo_cv);
    for (int y = 0; y < H; y++) {
        uint16_t *row = &s_pomo_buf[y * st];
        const uint16_t *col = &s_pomo_up[W - 1 - y];
        for (int x = 0; x < W; x++) row[x] = col[x * W];
    }
    lv_obj_invalidate(s_pomo_cv);
}

static void pomo_tick(lv_timer_t *t)
{
    pomo_draw();
}

static void pomo_zone(int z)
{
    if (z == PZ_GO) {
        if (s_po.run) {
            s_po.left = pomo_left();
            s_po.run = false;
        } else {
            if (s_po.left <= 0) s_po.left = POMO_LEN[s_po.phase];
            s_po.end_us = esp_timer_get_time() + (int64_t)s_po.left * 1000000;
            s_po.run = true;
            audio_tone(1047, 60, 0);
        }
    } else if (z == PZ_RESET) {
        if (!s_po.run && s_po.left == POMO_LEN[s_po.phase]) {   // já estava no início: recomeça o ciclo
            s_po.phase = POMO_FOCUS;
            s_po.done = 0;
        }
        s_po.run = false;
        s_po.left = POMO_LEN[s_po.phase];
    } else if (z == PZ_SKIP) {
        bool was = s_po.run;
        pomo_next(false);
        if (was && s_po.phase != POMO_FOCUS) {   // saltar um foco a contar começa logo a pausa
            s_po.run = true;
            s_po.end_us = esp_timer_get_time() + (int64_t)s_po.left * 1000000;
        }
    }
    pomo_draw();
}

static void pomo_ev(lv_event_t *e)
{
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    pomo_zone(anim_pomo_zone(W - 1 - pt.y, pt.x));   // ponto do painel -> coordenadas de quem olha
}

static void pomo_hide(void)
{
    if (s_pomo_tm) lv_timer_delete(s_pomo_tm);
    s_pomo_tm = NULL;
    if (s_pomo) lv_obj_delete(s_pomo);
    s_pomo = s_pomo_cv = NULL;
    ANIM(anim_out16(s_pomo_up, false));
    heap_caps_free(s_pomo_buf);
    heap_caps_free(s_pomo_up);
    s_pomo_buf = s_pomo_up = NULL;
    screen_cover();
}

static void pomo_show(void)
{
    if (s_pomo) return;
    s_pomo_buf = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    s_pomo_up = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!s_pomo_buf || !s_pomo_up) {
        pomo_hide();
        return;
    }
    ANIM(anim_out16(s_pomo_up, true));
    s_pomo = box(lv_layer_top(), 0x000000, 0);   // por cima de tudo menos dos avisos que vierem depois
    lv_obj_set_size(s_pomo, W, H);
    lv_obj_add_flag(s_pomo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pomo, pomo_ev, LV_EVENT_SHORT_CLICKED, NULL);
    s_pomo_cv = lv_canvas_create(s_pomo);
    lv_canvas_set_buffer(s_pomo_cv, s_pomo_buf, W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(s_pomo_cv, LV_OBJ_FLAG_CLICKABLE);
    screen_cover();
    s_pomo_t0 = esp_timer_get_time();
    s_pomo_tm = lv_timer_create(pomo_tick, 200, NULL);
    pomo_draw();
}

// a cada segundo: fim da fase (mesmo com o ecrã fechado)
static void pomo_check(void)
{
    if (!s_po.run || esp_timer_get_time() < s_po.end_us) return;
    bool was_focus = s_po.phase == POMO_FOCUS;
    pomo_next(true);
    for (int i = 0; i < 2; i++) {   // fim do foco: a subir; fim da pausa: a descer
        audio_tone(was_focus ? 784 : 1319, 150, 0);
        audio_tone(988, 150, 0);
        audio_tone(was_focus ? 1319 : 784, 280, 0);
        audio_tone(0, 250, 0);
    }
    wake_nolock();
    if (s_pomo) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "kind", "alarm");
    cJSON_AddStringToObject(m, "from", was_focus ? "POMODORO" : "AO TRABALHO");
    cJSON_AddStringToObject(m, "title", was_focus ? "Pomodoro concluído" : "Fim da pausa");
    cJSON_AddStringToObject(m, "text", was_focus ? (s_po.phase == POMO_LONG ? "Pausa longa de 15 min" : "Pausa de 5 min")
                                                 : "Gire o cubo para a direita para começar o foco");
    cJSON_AddBoolToObject(m, "sound", false);
    cJSON_AddBoolToObject(m, "local", true);
    notify_show(m);
    cJSON_Delete(m);
}

// ------------------------------------------------------------------------------------- Varanda do Romeu
// O cubo em pé girado 90° para a esquerda abre a varanda (varanda.c). Desenha-se direita para quem olha e roda-se:
// o topo de quem olha é o lado direito do painel.
static fr_t s_var_fr;
static uint16_t *s_var_up;
static lv_timer_t *s_var_tm;
static int64_t s_var_t0;
static bool s_var_chg;

static void var_draw_fn(fr_t *f, uint32_t *buf, int stride)
{
    varanda_draw(s_var_up, f->t);
    uint16_t *o = (uint16_t *)buf;
    for (int y = 0; y < H; y++) {
        uint16_t *row = &o[y * stride];
        for (int x = 0; x < W; x++) row[x] = s_var_up[(W - 1 - x) * W + y];
    }
}

static void var_tick(lv_timer_t *t)
{
    float el = (esp_timer_get_time() - s_var_t0) / 1e6f;
    if (fr_step(&s_var_fr, el, 0.05f)) fr_submit(&s_var_fr, el);
    power_info_t p;
    power_get(&p);
    if (p.charging && !s_var_chg) varanda_event(VEV_CHARGE, 0, 0, NULL, NULL);
    s_var_chg = p.charging;
    lv_display_trigger_activity(NULL);
}

static void var_ev(lv_event_t *e)
{
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    varanda_event(VEV_TOUCH, pt.y, W - 1 - pt.x, NULL, NULL);   // ponto do painel -> coordenadas de quem olha
}

static void var_hide(void)
{
    if (!s_var) return;
    if (s_var_tm) lv_timer_delete(s_var_tm);
    s_var_tm = NULL;
    fr_close(&s_var_fr);
    lv_obj_delete(s_var);
    s_var = NULL;
    ANIM(anim_out16(s_var_up, false));
    heap_caps_free(s_var_up);
    s_var_up = NULL;
    audio_mic_local(false);
    link_send_event("varanda", "open", "0", NULL, NULL);
    screen_cover();
}

static void var_show(void)
{
    if (s_var) return;
    if (!varanda_load()) {   // ainda sem imagens: o dashd envia-as e manda {"t":"varanda","reload":true}
        link_send_event("varanda", "open", "1", "need", "1");
        toast_nolock("A preparar a varanda do Romeu...", "info");
        return;
    }
    s_var_up = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!s_var_up) return;
    ANIM(anim_out16(s_var_up, true));
    s_var = box(lv_layer_top(), 0x000000, 0);
    lv_obj_set_size(s_var, W, H);
    lv_obj_add_flag(s_var, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_var, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_var, var_ev, LV_EVENT_PRESSED, NULL);
    ANIM(varanda_reset());
    if (!fr_open(&s_var_fr, s_var, W, H, var_draw_fn)) {
        lv_obj_delete(s_var);
        s_var = NULL;
        return;
    }
    power_info_t p;
    power_get(&p);
    s_var_chg = p.charging;
    audio_mic_local(true);
    s_var_t0 = esp_timer_get_time();
    s_var_tm = lv_timer_create(var_tick, 15, NULL);
    var_tick(NULL);
    screen_cover();
    link_send_event("varanda", "open", "1", NULL, NULL);
}

bool ui_rotated_open(void)
{
    return (s_var || s_pomo) && !s_rot_test;
}

void ui_imu_event(int ev)
{
    if (s_var) varanda_event(ev == 1 ? VEV_KNOCK : VEV_SHAKE, 0, 0, NULL, NULL);
}

void ui_orient(int ori)
{
    if (!bsp_display_lock(500)) return;
    s_var_want = ori == 2;
    s_rot_test = false;
    if (ori == 1) pomo_show();
    else pomo_hide();
    if (ori == 2) var_show();
    else var_hide();
    wake_nolock();
    bsp_display_unlock();
}

bool ui_pomo_key(int key, int ev)
{
    bool used = false;
    if (bsp_display_lock(500)) {
        if (s_pomo && key == 0 && ev == 0) {   // BOOT curto: iniciar / pausar
            pomo_zone(PZ_GO);
            used = true;
        }
        if (s_var && key == 0 && ev == 0) {    // BOOT curto na varanda: festas no Romeu
            varanda_event(VEV_PET, 0, 0, NULL, NULL);
            used = true;
        }
        bsp_display_unlock();
    }
    return used;
}

// ------------------------------------------------------------------------------------ "Sobre"
// Aurora boreal ou hiperespaço (anim.c, alternam a cada toque no nome) com os créditos numa faixa escura em
// baixo, como no DashDeck: aparecem depois do nome (aurora) ou do salto, e somem ao fim de 7 s. Um toque fecha (se a cena não o usar); fecha sozinha ao fim de ABOUT_SECS.
#define ABOUT_SECS 45
#define ABOUT_CREDITS_S 7.0f
static fr_t s_about_fr;
static float s_about_until, s_about_cred;
static char s_host_ver[40];

static void about_draw_fn(fr_t *f, uint32_t *buf, int stride)
{
    anim_about_draw(buf, f->w, f->h, stride, f->t);
}

static void about_close(void)
{
    if (s_about_tm) lv_timer_delete(s_about_tm);
    s_about_tm = NULL;
    fr_close(&s_about_fr);
    if (s_about) lv_obj_delete(s_about);
    s_about = s_about_txt = NULL;
    screen_cover();
}

static void about_tick(lv_timer_t *t)
{
    float el = (esp_timer_get_time() - s_about_t0) / 1e6f;
    if (el > s_about_until) {
        about_close();
        return;
    }
    if (fr_step(&s_about_fr, el, 0)) fr_submit(&s_about_fr, el);
    // os créditos aparecem em s_about_cred (0,4 s) e somem (0,6 s) ABOUT_CREDITS_S depois
    if (s_about_txt && s_about_cred >= 0) {
        float c = el - s_about_cred;
        if (c >= 0 && c < ABOUT_CREDITS_S + 0.6f) {
            float a = c < 0.4f ? c / 0.4f : c < ABOUT_CREDITS_S ? 1 : 1 - (c - ABOUT_CREDITS_S) / 0.6f;
            lv_obj_remove_flag(s_about_txt, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(s_about_txt, (lv_opa_t)(255 * a), 0);
        } else if (c >= ABOUT_CREDITS_S + 0.6f) {
            lv_obj_add_flag(s_about_txt, LV_OBJ_FLAG_HIDDEN);
            s_about_cred = -1;   // acabou
        }
    }
    lv_display_trigger_activity(NULL);
}

static void about_tap(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_wait_release(in);   // o toque não passa para a tela por baixo
    lv_point_t p;
    lv_indev_get_point(in, &p);
    float el = (esp_timer_get_time() - s_about_t0) / 1e6f;
    bool used;
    anim_lock();
    used = anim_about_touch(p.x, p.y, el);
    anim_unlock();
    if (used) {
        lv_obj_add_flag(s_about_txt, LV_OBJ_FLAG_HIDDEN);
        s_about_cred = -1;
        s_about_until = el + 30;
        return;
    }
    about_close();
}

static void about_show(void)
{
    if (s_about || s_notif) return;
    s_about = box(lv_layer_top(), 0x000000, 0);
    lv_obj_set_size(s_about, W, H);
    lv_obj_add_flag(s_about, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_about, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_about, about_tap, LV_EVENT_PRESSED, NULL);
    static int about_n;   // cada toque no nome mostra a outra cena (aurora, hiperespaço, ...)
    ANIM(anim_about_start(about_n++ & 1, W, H));
    if (!fr_open(&s_about_fr, s_about, W, H, about_draw_fn)) {
        lv_obj_delete(s_about);
        s_about = NULL;
        ui_toast("DashDeck\xC2\xB3 " DECK3_FW_VERSION " por CT7BRA", "info");
        return;
    }
    // créditos numa faixa escura em baixo
    s_about_txt = box(s_about, 0x05070C, 0);
    lv_obj_set_style_bg_opa(s_about_txt, LV_OPA_60, 0);
    lv_obj_set_size(s_about_txt, W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(s_about_txt, 12, 0);
    lv_obj_set_style_pad_bottom(s_about_txt, 18, 0);
    lv_obj_set_flex_flow(s_about_txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_about_txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_about_txt, 4, 0);
    lv_obj_remove_flag(s_about_txt, LV_OBJ_FLAG_CLICKABLE);
    char ver[72];
    snprintf(ver, sizeof(ver), "firmware %s%s%s", DECK3_FW_VERSION, s_host_ver[0] ? " \xC2\xB7 host " : "", s_host_ver);
    label(s_about_txt, "por CT7BRA", &deck_font_20, 0xFFFFFF);
    label(s_about_txt, "github.com/tiuksferve/DashDeck", &deck_font_16, 0x7CC4FF);
    label(s_about_txt, ver, &deck_font_16, 0xB8C2D0);
    const cJSON *ln;
    cJSON_ArrayForEach(ln, s_about_lines) {
        if (cJSON_IsString(ln)) label(s_about_txt, ln->valuestring, &deck_font_16, 0x8A94A8);
    }
    lv_obj_align(s_about_txt, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_about_txt, LV_OBJ_FLAG_HIDDEN);   // só depois do nome ou do salto
    s_about_t0 = esp_timer_get_time();
    s_about_until = ABOUT_SECS;
    s_about_cred = anim_about_credits_at();
    s_about_tm = lv_timer_create(about_tick, 15, NULL);
    about_tick(NULL);
    screen_cover();
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
EXT_RAM_BSS_ATTR static char s_shot_line[2600];

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
    EXT_RAM_BSS_ATTR static uint8_t raw[1800];
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

// {"t":"perf"}: quadros por segundo e tempo de cada refresco desde o último pedido (testes de desempenho)
static uint32_t s_pf_n;
static int64_t s_pf_t0, s_pf_since, s_pf_busy, s_pf_w0, s_pf_wait;

static void perf_ev(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FLUSH_WAIT_START) s_pf_w0 = now;
    else if (code == LV_EVENT_FLUSH_WAIT_FINISH) s_pf_wait += now - s_pf_w0;
    else if (code == LV_EVENT_REFR_START) s_pf_t0 = now;
    else if (s_pf_t0) {
        s_pf_busy += now - s_pf_t0;
        s_pf_n++;
    }
}

static void perf_report(void)
{
    static bool hooked;
    int64_t now = esp_timer_get_time();
    if (!hooked) {
        lv_display_add_event_cb(lv_display_get_default(), perf_ev, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(lv_display_get_default(), perf_ev, LV_EVENT_REFR_READY, NULL);
        lv_display_add_event_cb(lv_display_get_default(), perf_ev, LV_EVENT_FLUSH_WAIT_START, NULL);
        lv_display_add_event_cb(lv_display_get_default(), perf_ev, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
        hooked = true;
    } else {
        char buf[128];
        float secs = (now - s_pf_since) / 1e6f;
        snprintf(buf, sizeof(buf), "{\"t\":\"perf\",\"fps\":%.1f,\"refr_ms\":%.1f,\"draw_ms\":%.1f,\"wait_ms\":%.1f,\"secs\":%.1f}",
                 s_pf_n / secs, s_pf_n ? s_pf_busy / 1000.0f / s_pf_n : 0.0f,
                 s_pf_dn ? s_pf_draw / 1000.0f / s_pf_dn : 0.0f, s_pf_n ? s_pf_wait / 1000.0f / s_pf_n : 0.0f, secs);
        link_send_text(buf);
    }
    s_pf_n = s_pf_dn = 0;
    s_pf_busy = s_pf_draw = s_pf_wait = 0;
    s_pf_since = now;
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
    const char *cl[] = {"notify", "audio", "mic", "imu", "keys", "ota", "confirm", "slider", "chart", "bat", "wifi_scan", "about", "sd", "image", "away"};
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

// ---------------------------------------------------------------------------- JPEG (câmara)
// {"t":"jpg","id":tela,"off":n,"size":total,"d":"base64","last":bool}: o dashd envia um JPEG do tamanho do
// canvas do plot "live" em pedaços; no último, é desenhado (plot_jpg) e o cubo responde
// {"t":"jpgack","ok":bool,"ms":tempo}. O dashd só envia o quadro seguinte depois da resposta.
#define JPG_MAX (64 * 1024)
static uint8_t *s_jpg;          // JPEG a receber (PSRAM; os pedaços chegam na tarefa do link)
static size_t s_jpg_len, s_jpg_cap, s_jpg_pos;

static void jpg_ack(bool ok, int ms)
{
    char b[64];
    snprintf(b, sizeof(b), "{\"t\":\"jpgack\",\"ok\":%s,\"ms\":%d}", ok ? "true" : "false", ms);
    link_send_text(b);
}

static void handle_jpg(const cJSON *msg)
{
    const cJSON *jo = cJSON_GetObjectItem(msg, "off"), *js = cJSON_GetObjectItem(msg, "size");
    int32_t off = cJSON_IsNumber(jo) ? (int32_t)jo->valuedouble : 0;
    if (off == 0) {
        int32_t size = cJSON_IsNumber(js) ? (int32_t)js->valuedouble : 0;
        if (size <= 0 || size > JPG_MAX) {
            jpg_ack(false, 0);
            return;
        }
        if ((size_t)size > s_jpg_cap) {
            heap_caps_free(s_jpg);
            s_jpg = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
            s_jpg_cap = s_jpg ? (size_t)size : 0;
            if (!s_jpg) {
                jpg_ack(false, 0);
                return;
            }
        }
        s_jpg_len = size;
        s_jpg_pos = 0;
    }
    const char *d = jstr(msg, "d") ? jstr(msg, "d") : "";
    size_t olen = 0;
    if (!s_jpg || off < 0 || (size_t)off != s_jpg_pos || (size_t)off >= s_jpg_len ||
        mbedtls_base64_decode(s_jpg + off, s_jpg_len - off, &olen, (const unsigned char *)d, strlen(d)) != 0) {
        s_jpg_pos = 0;
        jpg_ack(false, 0);
        return;
    }
    s_jpg_pos = off + olen;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(msg, "last"))) return;   // os pedaços do meio não têm resposta
    bool ok = false;
    int64_t t0 = esp_timer_get_time();
    if (s_jpg_pos == s_jpg_len && bsp_display_lock(1000)) {
        const char *id = jstr(msg, "id");
        if (id && !strcmp(id, s_id)) ok = plot_jpg(id, s_jpg, s_jpg_len);
        bsp_display_unlock();
    }
    s_jpg_pos = 0;
    jpg_ack(ok, (int)((esp_timer_get_time() - t0) / 1000));
}

// {"t":"mem"}: memória (interna, DMA, PSRAM: livre, maior bloco, mínimo de sempre) e, por tarefa, a folga mínima
// da stack e se está na PSRAM. Para medir antes de cortar stacks.
static void mem_report(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "mem");
    const uint32_t caps[3] = {MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_DMA, MALLOC_CAP_SPIRAM};
    const char *nm[3] = {"int", "dma", "psram"};
    for (int i = 0; i < 3; i++) {
        cJSON *a = cJSON_AddArrayToObject(o, nm[i]);   // [livre, maior bloco, mínimo de sempre]
        cJSON_AddItemToArray(a, cJSON_CreateNumber(heap_caps_get_free_size(caps[i])));
        cJSON_AddItemToArray(a, cJSON_CreateNumber(heap_caps_get_largest_free_block(caps[i])));
        cJSON_AddItemToArray(a, cJSON_CreateNumber(heap_caps_get_minimum_free_size(caps[i])));
    }
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = heap_caps_malloc(sizeof(TaskStatus_t) * (n + 4), MALLOC_CAP_SPIRAM);
    if (ts) {
        n = uxTaskGetSystemState(ts, n + 4, NULL);
        cJSON *a = cJSON_AddArrayToObject(o, "tasks");   // [nome, folga mínima (bytes), stack na PSRAM]
        for (UBaseType_t i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateArray();
            cJSON_AddItemToArray(e, cJSON_CreateString(ts[i].pcTaskName));
            cJSON_AddItemToArray(e, cJSON_CreateNumber(ts[i].usStackHighWaterMark));
            cJSON_AddItemToArray(e, cJSON_CreateNumber(esp_ptr_external_ram(ts[i].pxStackBase) ? 1 : 0));
            cJSON_AddItemToArray(a, e);
        }
        heap_caps_free(ts);
    }
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
    if (!strcmp(t, "bg")) { bg_handle(msg); return; }   // fundo do mapa ou do relógio (sem o LVGL)
    if (!strcmp(t, "jpg")) { handle_jpg(msg); return; }   // quadros da câmara (o lock só no fim)

    if (!strcmp(t, "mem")) { mem_report(); return; }   // memória e folga das stacks (sem o lock do LVGL)
    if (!strcmp(t, "dbg")) {   // {"t":"dbg"}: estado dos quadros e do trinco do anim.c (sem o lock do LVGL)
        char b[200];
        TaskHandle_t h = anim_lock_holder();
        snprintf(b, sizeof(b), "{\"t\":\"dbg\",\"fr\":[%d,%d,%d],\"anim\":\"%s\",\"frq\":%d,\"pf\":%u}",
                 s_boot_fr.state, s_clk_fr.state, s_about_fr.state, h ? pcTaskGetName(h) : "",
                 (int)uxQueueMessagesWaiting(s_frq), (unsigned)s_pf_dn);
        link_send_text(b);
        return;
    }
    if (!bsp_display_lock(2000)) {
        ESP_LOGW(TAG, "lock do LVGL ocupado; '%s' descartada", t);
        return;
    }
    if (!strcmp(t, "emoji")) handle_emoji(msg);
    else if (!strcmp(t, "screen")) show_screen(msg);
    else if (!strcmp(t, "update")) update_screen(msg);
    else if (!strcmp(t, "shot")) shot_take();
    else if (!strcmp(t, "perf")) perf_report();
    else if (!strcmp(t, "clkface")) {   // {"t":"clkface","v":n}: muda a face do relógio (testes e capturas)
        s_clk_face = ((jint(msg, "v", 0) % ANIM_CLOCK_FACES) + ANIM_CLOCK_FACES) % ANIM_CLOCK_FACES;
        store_set("clkface", s_clk_face);
        if (s_clk) clock_face_name();
    }
    else if (!strcmp(t, "pomo")) {   // {"t":"pomo","show":true|false,"z":PZ_*,"left":s}: testes sem girar o cubo
        const cJSON *sh = cJSON_GetObjectItem(msg, "show");
        if (cJSON_IsTrue(sh)) { s_rot_test = true; pomo_show(); }
        if (cJSON_IsFalse(sh)) pomo_hide();
        int z = jint(msg, "z", PZ_NONE);
        if (z > PZ_NONE && z <= PZ_SKIP) pomo_zone(z);
        int left = jint(msg, "left", -1);   // encurta a fase atual (para ver o fim sem esperar)
        if (left >= 0) {
            s_po.left = left;
            if (s_po.run) s_po.end_us = esp_timer_get_time() + (int64_t)left * 1000000;
            if (s_pomo) pomo_draw();
        }
    }
    else if (!strcmp(t, "bootanim")) {   // {"t":"bootanim"}: repete a sequência de arranque (testes e capturas)
        if (s_screen && !s_boot_cv) {
            s_boot_first = true;
            boot_show_mode(false);
            s_boot_exit = true;   // a tela atual volta no fim da sequência
        }
    }
    else if (!strcmp(t, "clkzone")) {   // {"t":"clkzone","v":z}: toque numa zona CZ_* do temporizador (testes)
        int z = jint(msg, "v", CZ_NONE);
        if (z > CZ_NONE && z <= CZ_AON) clock_zone(z);
    }
    else if (!strcmp(t, "toast")) toast_nolock(jstr(msg, "text"), jstr(msg, "level"));
    else if (!strcmp(t, "notify")) notify_show(msg);
    else if (!strcmp(t, "status")) strlcpy(s_status, jstr(msg, "text") ? jstr(msg, "text") : "", sizeof(s_status));
    else if (!strcmp(t, "bl")) {
        int v = jint(msg, "v", s_bl);
        if (v <= 0) bl_off_nolock();   // 0% = desligado (o brilho guardado fica o anterior)
        else {
            s_bl = v < 2 ? 2 : v > 100 ? 100 : v;
            if (s_wake_pane) {
                lv_obj_delete(s_wake_pane);
                s_wake_pane = NULL;
            }
            bl_set(s_bl);
            s_bl_state = 0;
            store_set("bl", s_bl);
        }
    } else if (!strcmp(t, "idle")) {
        s_dim_s = jint(msg, "dim", s_dim_s);
        s_off_s = jint(msg, "off", s_off_s);
    } else if (!strcmp(t, "wake")) wake_nolock();
    else if (!strcmp(t, "varanda")) {   // dados do dashd para a varanda; "reload": imagens novas no cartão
        varanda_event_json(msg);
        const cJSON *sh = cJSON_GetObjectItem(msg, "show");   // testes sem girar o cubo
        if (cJSON_IsTrue(sh)) { s_var_want = true; s_rot_test = true; var_show(); }
        if (cJSON_IsFalse(sh)) { s_var_want = false; var_hide(); }
        const char *ev = jstr(msg, "ev");                      // testes: knock shake clap noise charge pet
        if (ev && s_var) {
            static const char *EVN[] = {"knock", "shake", "clap", "noise", "charge", "pet"};
            static const int EVT[] = {VEV_KNOCK, VEV_SHAKE, VEV_CLAP, VEV_NOISE, VEV_CHARGE, VEV_PET};
            for (int i = 0; i < 6; i++)
                if (!strcmp(ev, EVN[i])) varanda_event(EVT[i], 0, 0, NULL, NULL);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "reload")) && !s_var) {
            ANIM(varanda_unload());
            if (s_var_want) var_show();
        }
    }
    else if (!strcmp(t, "about")) {   // {"t":"about","lines":[...],"show":true}
        const cJSON *l = cJSON_GetObjectItem(msg, "lines");
        if (cJSON_IsArray(l)) {
            cJSON_Delete(s_about_lines);
            s_about_lines = cJSON_Duplicate(l, true);
        }
        const char *hv = jstr(msg, "host");
        if (hv) strlcpy(s_host_ver, hv, sizeof(s_host_ver));
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "show"))) about_show();
    }
    else if (!strcmp(t, "rot")) {
        // experimental: a rotação do toque ainda tem de ser confirmada na placa
        bsp_display_rotation_set((bsp_display_rotation_t)(jint(msg, "v", 0) & 3));
    } else if (!strcmp(t, "offline")) boot_show();
    bsp_display_unlock();
}

// PWR sem host: alterna o brilho localmente (0/25/50/75/100; 0 = desligado) e guarda-o
void ui_local_bl_cycle(void)
{
    static const int lv[] = {0, 25, 50, 75, 100};   // como o bl_levels do dashd
    int cur = s_bl_state == 3 ? 0 : s_bl, n = 0;
    for (int i = 0; i < 5; i++) if (cur >= lv[i]) n = i + 1;
    int v = lv[n % 5];
    if (!bsp_display_lock(500)) return;
    if (v == 0) bl_off_nolock();
    else {
        s_bl = v;
        if (s_wake_pane) {
            lv_obj_delete(s_wake_pane);
            s_wake_pane = NULL;
        }
        bl_set(s_bl);
        s_bl_state = 0;
        char t[24];
        snprintf(t, sizeof(t), "Brilho %d%%", s_bl);
        toast_nolock(t, "info");
    }
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
    clock_check();
    pomo_check();
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
    clock_load();
    s_po.today = store_get("po_today", 0);
    s_po.yday = store_get("po_yday", -1);
    bsp_display_lock(0);   // 0 = esperar o tempo que for preciso
    anim_init();
    emoji_fonts_init();
    s_frq = xQueueCreate(4, sizeof(fr_t *));
    xTaskCreatePinnedToCoreWithCaps(fr_task, "frames", 6144, NULL, 4, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);   // stack na PSRAM
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
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -14, 6);   // longe do canto arredondado do painel
    s_link = box(s_hdr, C_ERR, LV_RADIUS_CIRCLE);
    lv_obj_set_size(s_link, 8, 8);
    lv_obj_align_to(s_link, s_clock, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    s_bat = label(s_hdr, "", &deck_font_16, C_SUB);
    lv_obj_align(s_bat, LV_ALIGN_BOTTOM_RIGHT, -10, -2);
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
    lv_timer_create(bl_apply_cb, 20, NULL);
    hdr_refresh();
    bsp_display_unlock();
    bl_set(s_bl);
}
