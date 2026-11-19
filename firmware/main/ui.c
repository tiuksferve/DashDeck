// DashDeck UI: renderizador genérico controlado pelo host (dashd no uConsole).
//
// Host -> ESP
//   (descrição completa: docs/arquitetura.md)
//   {"t":"screen","id":"home","tpl":"menu|list|kv|log|text|plot","title":"..","back":false,
//    "items":[{"id","label","sub","icon","color","right"}], "rows":[["k","v","#color"?]],
//    "lines":[".."], "text":"..", "buttons":[{"id","label","color","confirm"}]}
//   {"t":"update","id":"..", <mesmos campos de dados>, "append":[".."]}
//   {"t":"status","text":"12:34"}   {"t":"toast","text":"..","level":"ok|warn|err|info"}
//   {"t":"bl","v":0..100}           {"t":"ping"}   {"t":"hello?"}
//   Páginas: "pages":N,"page":i no screen/update mostram pontos; o swipe lateral envia "swipe".
//   tpl "plot": canvas w x h (px) + linhas kv ("rows") ao lado (se couber) ou por baixo:
//    "plot":{"w":308,"h":136,"cx":..,"cy":..,"R":..,"rings":[px,..],"rlabels":["","50nm"],
//            "cardinal":true,"cross":true,"center":true,"msg":"..","bg":"chave",
//            "pts":[[x,y,"#cor","rótulo",rumo|null,cheio(0/1)],..],   coordenadas em px do canvas
//            "tracks":[["#cor",largura(1|2),"xxyyxxyy.."],..],
//            "ctrl":[["id","tl|tr|bl|br|l|r|t|b","rótulo"],..]}  botões por cima do canvas (ex.: setas
//            e zoom do mapa) -> {"t":"btn","id":screen,"btn":id}; rótulo vazio nas bordas = seta  trajetos por baixo dos pontos; cada
//            coordenada são 2 caracteres base64url (A-Z a-z 0-9 - _) com valor px+1024
//   {"t":"bg","key":"..","w":..,"h":..,"pal":["#rrggbb"x16],"off":n,"d":"base64","last":true}
//      imagem de fundo 4 bpp (2 px/byte, nibble alto primeiro) usada pelo plot com o mesmo "bg";
//      cada pedaço é confirmado com {"t":"bgack","off":n,"ok":bool}
//   {"t":"rot","v":0..3}  grava a rotação na NVS e reinicia
//   {"t":"icon","id":"..","w":32,"h":32,"d":"base64 RGB565A8"} -> {"t":"iconack","id":..,"ok":..}
//   {"t":"dock","items":[{"id","label","icon","img","color"}]}  barra fixa em baixo -> {"t":"dock","item":id}
//   screen: "layout":"grid" (menu em boxes) e "dock":false (esconde o dock nesta tela)
//   {"t":"emoji","cp":0x100000+i,"w":15,"h":14,"d":"base64 RGB565A8"} -> {"t":"emojiack","cp":..,"ok":..}
//      imagem do emoji que o host pôs no texto como o código privado cp (até 48)
//   item de menu: "badge":"3" mostra um selo vermelho (ex.: mensagens não lidas); nos boxes,
//      "bg" (fundo) e "sub_color" (cor do subtítulo), ex.: interruptores do AIO v2
//   item de lista: "bg":"#rrggbb" fundo do card; "confirm":".." pede Não/Sim antes do "tap"
//      ícones guardados na RAM; um item de menu com "img":"id" mostra-o no lugar do símbolo
// ESP -> Host
//   {"t":"hello","fw":..,"w":320,"h":172,"rot":1,"heap":..}  {"t":"tap","id":screen,"item":id}
//   {"t":"btn","id":screen,"btn":id}       {"t":"back","id":screen}   {"t":"pong"}
//   {"t":"swipe","id":screen,"dir":"left|right"}
// {"t":"idle","home":segundos}  apaga o ecrã (backlight 0) após N s sem toques na tela "home"; 0 = nunca.
//   O toque que acorda (ou o BOOT) não chega ao que está por baixo.
// {"t":"notify","from":..,"text":..}  aviso de mensagem direta (animação "Ondas LoRa"); um toque
//   -> {"t":"notify","act":"open"}
// Relógio (tpl "clock"): ecrã inteiro, sem barra nem dock. Desenho em clockface.c.
//   {"t":"time","epoch":<segundos, hora local>}  acerta o relógio do ESP (que depois conta sozinho)
//   screen/update "clock":{"face":0|1,"pal":0..2,"bl":%,"bg":"chave","night":bool,"anim":"rain|storm|snow|",
//                          "wx":{"t":..,"max":..,"min":..,"rain":%,"wind":km/h},
//                          "icon":{"pal":["#rrggbb"x<=8],"role":[1|2,..],"d":"256 dígitos 0-7"}}
//   O fundo da paisagem chega por "bg" com "bpp":8 (paleta de 256 cores, 1 byte por px).
//   Swipe lateral troca o mostrador -> {"t":"clock","face":"1"}; vertical muda o brilho ->
//   {"t":"clock","bl":"60"}; um toque pede confirmação e o 2.º (em 2,5 s) volta -> {"t":"back"}.
//   "dim":s,"dim_bl":% no clock: escurece após s segundos sem toques; o toque seguinte repõe o brilho.
//   Os gestos do relógio são medidos do toque ao soltar (não pelo detetor do LVGL). O brilho normal volta ao sair.
#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bootanim.h"
#include "notify.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "clockface.h"
#include "dash_fonts.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"

static const char *TAG = "ui";

#define MAX_ITEMS   64
#define MAX_BTNS    8
#define ID_LEN      32
#define LOG_MAX     60
#define BAR_H       24
#define DOCK_H      36
#define DOCK_MAX    6

// Paleta
#define C_BG      0x0B0F14
#define C_BAR     0x111821
#define C_CARD    0x18202A
#define C_CARD_PR 0x26313E
#define C_TEXT    0xE6EDF3
#define C_SUB     0x8B98A5
#define C_ACCENT  0x3FA9F5
#define C_OK      0x3FB950
#define C_WARN    0xD29922
#define C_ERR     0xF85149

typedef enum { TPL_NONE, TPL_MENU, TPL_LIST, TPL_KV, TPL_LOG, TPL_TEXT, TPL_PLOT, TPL_CLOCK } tpl_t;
#define IS_KV(t) ((t) == TPL_KV || (t) == TPL_PLOT)

#define PLOT_PTS 80
#define PLOT_MAX_PX (320 * 172)
static lv_obj_t *s_canvas;
static lv_obj_t *s_plot_wrap;      // canvas + botões de controlo por cima
#define CTRL_MAX 10
static char s_ctrl_ids[CTRL_MAX][ID_LEN];
static char s_ctrl_sig[256];
static lv_draw_buf_t s_cbuf;
static uint8_t *s_cmem;
static int32_t s_cw, s_ch;
// Imagem de fundo (4 bpp + paleta de 16, ou 8 bpp + paleta de 256 no relógio)
static uint8_t *s_bg;
static size_t s_bg_size;
static int32_t s_bg_w, s_bg_h;
static uint8_t s_bg_bpp = 4;
static uint16_t s_bg_pal[256];
static char s_bg_key[32];
static bool s_bg_ready;
static size_t s_bg_next;     // próximo offset esperado
// Relógio
static lv_obj_t *s_clock;          // ecrã inteiro por cima de tudo
static lv_timer_t *s_clock_timer;
static clock_state_t s_cs;
static char s_clock_bg[32];
static bool s_clock_drawn_bg;      // já houve um fundo válido (durante um envio novo, congela)
static int s_clock_last_sec = -1;
static uint32_t s_bright_until, s_dots_until, s_exit_until;
static lv_point_t s_press_pt;
// Ecrã apagado por inatividade na tela inicial
static uint32_t s_sleep_ms;        // 0 = nunca
static bool s_asleep;
static lv_obj_t *s_shield;         // apanha o toque que acorda (não ativa o que está por baixo)
static bool s_dock_want;           // a tela atual quer o dock
static bool s_pressing;
static uint32_t s_dim_ms = 300000; // relógio: escurece após N ms sem toques (0 = nunca)
static uint8_t s_dim_bl = 10;
static bool s_dimmed;
static bool s_ovl_was;
static uint8_t s_bl = 80, s_bl_saved = 80;
static int64_t s_epoch_off;        // hora local = esp_timer + s_epoch_off (µs)
static bool s_has_time;
// Geometria lógica (depois da rotação)
static int32_t s_W = BOARD_LCD_H_RES, s_H = BOARD_LCD_V_RES;
static bool s_land;
static uint8_t s_rot;
static lv_obj_t *s_list;     // pai dos itens/linhas (s_body, ou caixa abaixo do plot)
static lv_obj_t *s_pager;
static int s_pages = 1, s_page;

static lv_obj_t *s_back_btn, *s_title, *s_status, *s_link;
static lv_obj_t *s_content, *s_body, *s_btns, *s_toast;
static lv_timer_t *s_toast_timer;

static char s_screen_id[ID_LEN];
static tpl_t s_tpl = TPL_NONE;
static bool s_has_screen;
static bool s_was_alive;

// Estado para atualizações no lugar
static int s_n;                                  // nº de itens/linhas no corpo
static char s_item_ids[MAX_ITEMS][ID_LEN];
static lv_obj_t *s_lbl_a[MAX_ITEMS];             // título / chave
static lv_obj_t *s_lbl_b[MAX_ITEMS];             // sub / valor
static lv_obj_t *s_lbl_c[MAX_ITEMS];             // ícone / texto à direita
static lv_obj_t *s_log_lbl;
static bool s_is_img[MAX_ITEMS];                 // s_lbl_c[i] é uma imagem (ícone de app)
static lv_obj_t *s_badge[MAX_ITEMS];             // selo vermelho no canto (menu)
static char s_btn_ids[MAX_BTNS][ID_LEN];

// Ícones de aplicações (RGB565A8) enviados pelo host
#define ICON_MAX 48
#define ICON_PX  48
typedef struct {
    char id[24];
    lv_image_dsc_t dsc;
    uint8_t *data;
} icon_t;
static icon_t s_icons[ICON_MAX];
static int s_icon_next;
static void make_badge(lv_obj_t *card, int i, const cJSON *it);
static void ask_confirm(const char *text, uint32_t yes_color);
static void draw_tracks(const cJSON *tracks);
static void build_ctrl(const cJSON *ctrl);

static lv_obj_t *s_dock;
static char s_dock_ids[DOCK_MAX][ID_LEN];
static bool s_dock_on;      // há itens no dock
static bool s_grid;         // menu atual em boxes
static bool s_hgrid;        // boxes numa fileira com scroll lateral (horizontal)
static int32_t s_content_h; // altura útil do conteúdo (sem margens)
static char s_btn_confirm[MAX_BTNS][96];
static int s_pending_btn = -1;
static char s_item_confirm[MAX_ITEMS][80];
static int s_pending_item = -1;
static char s_pending_item_id[ID_LEN];  // id guardado: um update pode reordenar os itens
static char s_pending_screen[ID_LEN];

// ---------- utilitários ----------

static const char *jstr(const cJSON *o, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

// strlcpy que não deixa um carácter UTF-8 cortado a meio no fim (apareceria como um quadrado)
static void utf8cpy(char *dst, const char *src, size_t size)
{
    size_t n = strlcpy(dst, src, size);
    if (n < size) return;
    size_t len = size - 1, i = len;
    while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) i--;   // bytes de continuação
    if (i > 0 && ((unsigned char)dst[i - 1] & 0x80)) {
        unsigned char lead = (unsigned char)dst[i - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (len - (i - 1) < need) dst[i - 1] = '\0';
    }
}

static bool jbool(const cJSON *o, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static lv_color_t parse_color(const char *s, uint32_t def)
{
    if (s && s[0] == '#' && strlen(s) == 7) {
        return lv_color_hex((uint32_t)strtoul(s + 1, NULL, 16));
    }
    if (s) {
        if (!strcmp(s, "ok")) return lv_color_hex(C_OK);
        if (!strcmp(s, "warn")) return lv_color_hex(C_WARN);
        if (!strcmp(s, "err")) return lv_color_hex(C_ERR);
        if (!strcmp(s, "sub")) return lv_color_hex(C_SUB);
        if (!strcmp(s, "accent")) return lv_color_hex(C_ACCENT);
    }
    return lv_color_hex(def);
}

static const char *icon_symbol(const char *name)
{
    static const struct { const char *n; const char *s; } map[] = {
        {"home", LV_SYMBOL_HOME},       {"settings", LV_SYMBOL_SETTINGS}, {"wifi", LV_SYMBOL_WIFI},
        {"gps", LV_SYMBOL_GPS},         {"play", LV_SYMBOL_PLAY},         {"stop", LV_SYMBOL_STOP},
        {"pause", LV_SYMBOL_PAUSE},     {"list", LV_SYMBOL_LIST},         {"bell", LV_SYMBOL_BELL},
        {"power", LV_SYMBOL_POWER},     {"refresh", LV_SYMBOL_REFRESH},   {"eye", LV_SYMBOL_EYE_OPEN},
        {"charge", LV_SYMBOL_CHARGE},   {"battery", LV_SYMBOL_BATTERY_FULL}, {"usb", LV_SYMBOL_USB},
        {"bluetooth", LV_SYMBOL_BLUETOOTH}, {"sd", LV_SYMBOL_SD_CARD},    {"folder", LV_SYMBOL_DIRECTORY},
        {"file", LV_SYMBOL_FILE},       {"mail", LV_SYMBOL_ENVELOPE},     {"audio", LV_SYMBOL_AUDIO},
        {"video", LV_SYMBOL_VIDEO},     {"image", LV_SYMBOL_IMAGE},       {"warning", LV_SYMBOL_WARNING},
        {"bars", LV_SYMBOL_BARS},       {"keyboard", LV_SYMBOL_KEYBOARD}, {"call", LV_SYMBOL_CALL},
        {"upload", LV_SYMBOL_UPLOAD},   {"download", LV_SYMBOL_DOWNLOAD}, {"loop", LV_SYMBOL_LOOP},
        {"edit", LV_SYMBOL_EDIT},       {"trash", LV_SYMBOL_TRASH},       {"ok", LV_SYMBOL_OK},
        {"close", LV_SYMBOL_CLOSE},     {"plus", LV_SYMBOL_PLUS},         {"drive", LV_SYMBOL_DRIVE},
        {"shuffle", LV_SYMBOL_SHUFFLE}, {"right", LV_SYMBOL_RIGHT},       {"volume", LV_SYMBOL_VOLUME_MAX},
    };
    if (!name) {
        return LV_SYMBOL_RIGHT;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(map[i].n, name)) {
            return map[i].s;
        }
    }
    return name;  // permite passar o próprio texto/símbolo
}

static lv_obj_t *plain_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

// ---------- toast ----------

static void toast_hide_cb(lv_timer_t *t)
{
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = NULL;
}

static void show_toast(const char *text, const char *level)
{
    uint32_t col = C_ACCENT;
    if (level && !strcmp(level, "ok")) col = C_OK;
    else if (level && !strcmp(level, "warn")) col = C_WARN;
    else if (level && !strcmp(level, "err")) col = C_ERR;
    lv_obj_set_style_border_color(s_toast, lv_color_hex(col), 0);
    lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) {
        lv_timer_reset(s_toast_timer);
    } else {
        s_toast_timer = lv_timer_create(toast_hide_cb, 2500, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
}

// ---------- eventos ----------

static void item_click_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n) {
        return;
    }
    if (s_item_confirm[idx][0]) {
        s_pending_item = idx;
        strlcpy(s_pending_item_id, s_item_ids[idx], ID_LEN);
        strlcpy(s_pending_screen, s_screen_id, ID_LEN);
        s_pending_btn = -1;
        ask_confirm(s_item_confirm[idx], C_ACCENT);
    } else {
        link_send_event("tap", "id", s_screen_id, "item", s_item_ids[idx]);
    }
}

static void confirm_cb(lv_event_t *e)
{
    lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    bool yes = lbl && !strcmp(lv_label_get_text(lbl), "Sim");
    if (yes && s_pending_btn >= 0) {
        link_send_event("btn", "id", s_screen_id, "btn", s_btn_ids[s_pending_btn]);
    }
    if (yes && s_pending_item >= 0 && !strcmp(s_pending_screen, s_screen_id)) {
        link_send_event("tap", "id", s_screen_id, "item", s_pending_item_id);
    }
    s_pending_btn = -1;
    s_pending_item = -1;
    lv_msgbox_close(mbox);
}

static void ask_confirm(const char *text, uint32_t yes_color)
{
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_obj_set_width(mbox, s_land ? 220 : 156);
    lv_msgbox_add_text(mbox, text);
    lv_obj_t *b1 = lv_msgbox_add_footer_button(mbox, "Não");
    lv_obj_t *b2 = lv_msgbox_add_footer_button(mbox, "Sim");
    lv_obj_set_style_bg_color(b2, lv_color_hex(yes_color), 0);
    lv_obj_add_event_cb(b1, confirm_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(b2, confirm_cb, LV_EVENT_CLICKED, mbox);
}

static void btn_click_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_BTNS) {
        return;
    }
    if (s_btn_confirm[idx][0]) {
        s_pending_btn = idx;
        s_pending_item = -1;
        ask_confirm(s_btn_confirm[idx], C_ERR);
    } else {
        link_send_event("btn", "id", s_screen_id, "btn", s_btn_ids[idx]);
    }
}

static void go_back(void)
{
    if (s_has_screen) {
        link_send_event("back", "id", s_screen_id, NULL, NULL);
    }
}

static void back_click_cb(lv_event_t *e)
{
    go_back();
}

// ---------- construção do corpo ----------

// ---------- emojis: imagens a cores dentro do texto ----------
// O host troca cada emoji (ou sequência: bandeira, tom de pele, ZWJ) por um código privado
// U+100000+i e envia a imagem antes da tela ({"t":"emoji"}). Uma imgfont do LVGL, posta no
// fallback das fontes dash_font_*, desenha esses códigos com a imagem guardada aqui.
#define EMOJI_MAX 48
#define EMOJI_CP0 0x100000
typedef struct {
    uint32_t cp;
    lv_image_dsc_t dsc;
    uint8_t *data;
} emoji_t;
static emoji_t s_emoji[EMOJI_MAX];

static const void *emoji_cb(const lv_font_t *font, uint32_t unicode, uint32_t next, int32_t *offset_y, void *ud)
{
    if (unicode < EMOJI_CP0 || unicode >= EMOJI_CP0 + EMOJI_MAX) return NULL;
    emoji_t *e = &s_emoji[unicode - EMOJI_CP0];
    if (!e->data || e->cp != unicode) return NULL;
    *offset_y = -2;   // assenta como uma letra com descendente
    return &e->dsc;
}

static void emoji_fonts_init(void)
{
    lv_font_t *fonts[] = {&dash_font_12, &dash_font_14, &dash_font_16, &dash_font_20};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        lv_font_t *im = lv_imgfont_create(14, emoji_cb, NULL);
        if (!im) continue;
        im->fallback = fonts[i]->fallback;   // depois dos emojis, os símbolos LV_SYMBOL_*
        fonts[i]->fallback = im;
    }
}

static void emoji_free_all(void)
{
    for (int i = 0; i < EMOJI_MAX; i++) {
        if (s_emoji[i].data) {
            lv_image_cache_drop(&s_emoji[i].dsc);
            free(s_emoji[i].data);
            s_emoji[i].data = NULL;
        }
    }
}

static int32_t jint(const cJSON *o, const char *key, int32_t def);

static void handle_emoji(const cJSON *msg)
{
    int32_t cp = jint(msg, "cp", 0), w = jint(msg, "w", 0), h = jint(msg, "h", 0);
    bool ok = false;
    if (cp >= EMOJI_CP0 && cp < EMOJI_CP0 + EMOJI_MAX && w > 0 && h > 0 && w <= 24 && h <= 24) {
        size_t size = (size_t)w * h * 3, olen = 0;
        uint8_t *buf = malloc(size);
        const char *d = jstr(msg, "d", "");
        if (buf && mbedtls_base64_decode(buf, size, &olen, (const unsigned char *)d, strlen(d)) == 0 && olen == size) {
            emoji_t *e = &s_emoji[cp - EMOJI_CP0];
            if (e->data) {
                lv_image_cache_drop(&e->dsc);
                free(e->data);
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
            free(buf);
        }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "emojiack");
    cJSON_AddNumberToObject(o, "cp", cp);
    cJSON_AddBoolToObject(o, "ok", ok);
    link_send(o);
    cJSON_Delete(o);
}

static const lv_image_dsc_t *icon_find(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < ICON_MAX; i++) {
        if (s_icons[i].data && !strcmp(s_icons[i].id, id)) return &s_icons[i].dsc;
    }
    return NULL;
}

static void handle_icon(const cJSON *msg)
{
    const char *id = jstr(msg, "id", "");
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(msg, "w"), *jh = cJSON_GetObjectItemCaseSensitive(msg, "h");
    int w = cJSON_IsNumber(jw) ? jw->valueint : 0, h = cJSON_IsNumber(jh) ? jh->valueint : 0;
    bool ok = false;
    if (id[0] && w > 0 && h > 0 && w <= ICON_PX && h <= ICON_PX) {
        size_t size = (size_t)w * h * 3, olen = 0;
        uint8_t *buf = malloc(size);
        const char *d = jstr(msg, "d", "");
        if (buf && mbedtls_base64_decode(buf, size, &olen, (const unsigned char *)d, strlen(d)) == 0 && olen == size) {
            icon_t *slot = NULL;
            for (int i = 0; i < ICON_MAX && !slot; i++) {
                if (s_icons[i].data && !strcmp(s_icons[i].id, id)) slot = &s_icons[i];
            }
            for (int i = 0; i < ICON_MAX && !slot; i++) {
                if (!s_icons[i].data) slot = &s_icons[i];
            }
            if (!slot) {                     // cheio: substitui de forma rotativa
                slot = &s_icons[s_icon_next];
                s_icon_next = (s_icon_next + 1) % ICON_MAX;
            }
            if (slot->data) {
                lv_image_cache_drop(&slot->dsc);
                free(slot->data);
            }
            strlcpy(slot->id, id, sizeof(slot->id));
            memset(&slot->dsc, 0, sizeof(slot->dsc));
            slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
            slot->dsc.header.w = w;
            slot->dsc.header.h = h;
            slot->dsc.header.stride = w * 2;
            slot->dsc.data_size = size;
            slot->dsc.data = buf;
            slot->data = buf;
            ok = true;
        } else {
            free(buf);
        }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "iconack");
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddBoolToObject(o, "ok", ok);
    link_send(o);
    cJSON_Delete(o);
}

static void build_menu_item(int i, const cJSON *it)
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, s_land ? (s_W - 12 - 6) / 2 : LV_PCT(100), 52);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 10, 0);
    lv_obj_set_style_pad_column(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    const lv_image_dsc_t *img = icon_find(jstr(it, "img", NULL));
    lv_obj_t *ic;
    if (img) {
        ic = lv_image_create(card);
        lv_image_set_src(ic, img);
        lv_obj_set_size(ic, 32, 32);
        lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_CENTER);
    } else {
        ic = mk_label(card, &dash_font_20, C_ACCENT, icon_symbol(jstr(it, "icon", NULL)));
        lv_obj_set_style_text_color(ic, parse_color(jstr(it, "color", NULL), C_ACCENT), 0);
        lv_obj_set_width(ic, 32);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    }
    s_is_img[i] = img != NULL;

    lv_obj_t *col = plain_box(card);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = mk_label(col, &dash_font_16, C_TEXT, jstr(it, "label", "?"));
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    const char *sub = jstr(it, "sub", "");
    lv_obj_t *s = mk_label(col, &dash_font_12, C_SUB, sub);
    lv_obj_set_width(s, LV_PCT(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    if (!sub[0]) {
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
    s_lbl_a[i] = t;
    s_lbl_b[i] = s;
    s_lbl_c[i] = ic;
    make_badge(card, i, it);
}

static void set_badge(int i, const cJSON *it)
{
    const char *b = jstr(it, "badge", "");
    if (!s_badge[i]) return;
    lv_label_set_text(lv_obj_get_child(s_badge[i], 0), b);
    if (b[0]) lv_obj_remove_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
}

static void make_badge(lv_obj_t *card, int i, const cJSON *it)
{
    lv_obj_t *b = lv_obj_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_min_width(b, 18, 0);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_pad_hor(b, 5, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ERR), 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 2, -4);
    lv_obj_t *l = mk_label(b, &dash_font_12, 0xFFFFFF, "");
    lv_obj_center(l);
    s_badge[i] = b;
    set_badge(i, it);
}

static lv_obj_t *icon_widget(lv_obj_t *parent, const cJSON *it, int i, const lv_font_t *font)
{
    const lv_image_dsc_t *img = icon_find(jstr(it, "img", NULL));
    lv_obj_t *ic;
    if (img) {
        ic = lv_image_create(parent);
        lv_image_set_src(ic, img);
        lv_obj_set_size(ic, 32, 32);
        lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_CENTER);
    } else {
        ic = mk_label(parent, font, C_ACCENT, icon_symbol(jstr(it, "icon", NULL)));
        lv_obj_set_style_text_color(ic, parse_color(jstr(it, "color", NULL), C_ACCENT), 0);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (i >= 0) s_is_img[i] = img != NULL;
    return ic;
}

// Box da grade: ícone em cima, nome em até 2 linhas, status numa linha.
// Box com "bg" (fundo) e "sub_color" (cor da linha de baixo), ex.: interruptores ligado/desligado.
static void tile_style(lv_obj_t *card, lv_obj_t *sub, const cJSON *it)
{
    lv_obj_set_style_bg_color(card, parse_color(jstr(it, "bg", NULL), C_CARD), 0);
    lv_obj_set_style_text_color(sub, parse_color(jstr(it, "sub_color", NULL), C_SUB), 0);
}

static void build_tile(int i, const cJSON *it)
{
    int cols = s_land ? 4 : 2;
    int32_t tw = s_hgrid ? 110 : (s_W - 12 - (cols - 1) * 6) / cols;
    const lv_font_t *nf = s_hgrid ? &dash_font_14 : &dash_font_12;
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, tw, s_hgrid ? s_content_h : LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_top(card, 6, 0);
    lv_obj_set_style_pad_bottom(card, 5, 0);
    lv_obj_set_style_pad_hor(card, 3, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *ic = icon_widget(card, it, i, &dash_font_20);
    if (!s_is_img[i]) {
        lv_obj_set_size(ic, 32, 32);
        lv_obj_set_style_pad_top(ic, 6, 0);
    }

    if (s_hgrid) {
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    int32_t lh = lv_font_get_line_height(&dash_font_12);
    lv_obj_t *t = mk_label(card, nf, C_TEXT, jstr(it, "label", "?"));
    lv_obj_set_size(t, LV_PCT(100), 2 * lv_font_get_line_height(nf));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    const char *sub = jstr(it, "sub", "");
    lv_obj_t *sl = mk_label(card, &dash_font_12, C_SUB, sub);
    lv_obj_set_size(sl, LV_PCT(100), lh);
    lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
    if (!sub[0]) lv_obj_add_flag(sl, LV_OBJ_FLAG_HIDDEN);
    s_lbl_a[i] = t;
    s_lbl_b[i] = sl;
    s_lbl_c[i] = ic;
    tile_style(card, sl, it);
    make_badge(card, i, it);
}

static void update_menu_item(int i, const cJSON *it)
{
    set_badge(i, it);
    if (s_grid) tile_style(lv_obj_get_parent(s_lbl_a[i]), s_lbl_b[i], it);
    lv_label_set_text(s_lbl_a[i], jstr(it, "label", "?"));
    const char *sub = jstr(it, "sub", "");
    lv_label_set_text(s_lbl_b[i], sub);
    if (sub[0]) lv_obj_remove_flag(s_lbl_b[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_lbl_b[i], LV_OBJ_FLAG_HIDDEN);
    const lv_image_dsc_t *img = icon_find(jstr(it, "img", NULL));
    if (s_is_img[i]) {
        if (img) lv_image_set_src(s_lbl_c[i], img);
    } else {
        lv_label_set_text(s_lbl_c[i], icon_symbol(jstr(it, "icon", NULL)));
        lv_obj_set_style_text_color(s_lbl_c[i], parse_color(jstr(it, "color", NULL), C_ACCENT), 0);
    }
}

static void build_list_item(int i, const cJSON *it)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, parse_color(jstr(it, "bg", NULL), C_CARD), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *col = plain_box(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = mk_label(col, &dash_font_14, C_TEXT, jstr(it, "label", ""));
    lv_obj_set_style_text_color(t, parse_color(jstr(it, "color", NULL), C_TEXT), 0);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_t *s = mk_label(col, &dash_font_12, C_SUB, jstr(it, "sub", ""));
    lv_obj_set_width(s, LV_PCT(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_t *r = mk_label(row, &dash_font_12, C_ACCENT, jstr(it, "right", ""));
    s_is_img[i] = false;
    s_lbl_a[i] = t;
    s_lbl_b[i] = s;
    s_lbl_c[i] = r;
}

static void update_list_item(int i, const cJSON *it)
{
    lv_label_set_text(s_lbl_a[i], jstr(it, "label", ""));
    lv_obj_set_style_text_color(s_lbl_a[i], parse_color(jstr(it, "color", NULL), C_TEXT), 0);
    lv_label_set_text(s_lbl_b[i], jstr(it, "sub", ""));
    lv_label_set_text(s_lbl_c[i], jstr(it, "right", ""));
    lv_obj_t *row = lv_obj_get_parent(s_lbl_c[i]);
    lv_obj_set_style_bg_color(row, parse_color(jstr(it, "bg", NULL), C_CARD), 0);
}

static void build_kv_row(int i, const cJSON *r)
{
    const cJSON *k = cJSON_GetArrayItem(r, 0), *v = cJSON_GetArrayItem(r, 1), *c = cJSON_GetArrayItem(r, 2);
    lv_obj_t *row = plain_box(s_list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(C_CARD_PR), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_lbl_a[i] = mk_label(row, &dash_font_12, C_SUB, cJSON_IsString(k) ? k->valuestring : "");
    s_lbl_b[i] = mk_label(row, &dash_font_14, C_TEXT, cJSON_IsString(v) ? v->valuestring : "");
    lv_obj_set_style_text_color(s_lbl_b[i], parse_color(cJSON_IsString(c) ? c->valuestring : NULL, C_TEXT), 0);
    lv_obj_set_style_text_align(s_lbl_b[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_max_width(s_lbl_b[i], LV_PCT(70), 0);
}

static void update_kv_row(int i, const cJSON *r)
{
    const cJSON *k = cJSON_GetArrayItem(r, 0), *v = cJSON_GetArrayItem(r, 1), *c = cJSON_GetArrayItem(r, 2);
    lv_label_set_text(s_lbl_a[i], cJSON_IsString(k) ? k->valuestring : "");
    lv_label_set_text(s_lbl_b[i], cJSON_IsString(v) ? v->valuestring : "");
    lv_obj_set_style_text_color(s_lbl_b[i], parse_color(cJSON_IsString(c) ? c->valuestring : NULL, C_TEXT), 0);
}

static void log_set_lines(const cJSON *lines, bool append)
{
    if (!s_log_lbl) {
        return;
    }
    // Junta texto existente (se append) + novas linhas e mantém as últimas LOG_MAX
    static char buf[4096];
    size_t len = 0;
    buf[0] = '\0';
    if (append) {
        len = strlcpy(buf, lv_label_get_text(s_log_lbl), sizeof(buf));
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    }
    const cJSON *ln;
    cJSON_ArrayForEach(ln, lines) {
        if (!cJSON_IsString(ln)) continue;
        int w = snprintf(buf + len, sizeof(buf) - len, "%s%s", len ? "\n" : "", ln->valuestring);
        if (w < 0) break;
        len += (size_t)w;
        if (len >= sizeof(buf) - 1) {
            len = sizeof(buf) - 1;
            break;
        }
    }
    // Corta do início: limite de linhas e de bytes (mantém metade do buffer livre para o próximo append)
    int count = 0;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') count++;
    char *start = buf;
    while ((count >= LOG_MAX || (size_t)(buf + len - start) > sizeof(buf) / 2) && (start = strchr(start, '\n'))) {
        start++;
        count--;
    }
    if (!start) start = buf;
    lv_label_set_text(s_log_lbl, start);
    lv_obj_update_layout(s_content);
    lv_obj_scroll_to_y(s_content, LV_COORD_MAX, LV_ANIM_OFF);
}

static void build_buttons(const cJSON *btns)
{
    lv_obj_clean(s_btns);
    memset(s_btn_ids, 0, sizeof(s_btn_ids));
    memset(s_btn_confirm, 0, sizeof(s_btn_confirm));
    int i = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, btns) {
        if (i >= MAX_BTNS) break;
        strlcpy(s_btn_ids[i], jstr(b, "id", ""), ID_LEN);
        utf8cpy(s_btn_confirm[i], jstr(b, "confirm", ""), sizeof(s_btn_confirm[i]));
        lv_obj_t *btn = lv_button_create(s_btns);
        lv_obj_set_height(btn, 36);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_min_width(btn, 70, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, parse_color(jstr(b, "color", NULL), C_ACCENT), 0);
        lv_obj_t *l = mk_label(btn, &dash_font_14, 0xFFFFFF, jstr(b, "label", "?"));
        lv_obj_center(l);
        lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        i++;
    }
    if (i) lv_obj_remove_flag(s_btns, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_btns, LV_OBJ_FLAG_HIDDEN);
}

// ---------- plot polar ----------

static void plot_text(lv_layer_t *layer, const char *txt, int32_t x, int32_t y, int32_t w, lv_color_t col,
                      lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;
    d.font = &dash_font_12;
    d.color = col;
    d.align = align;
    lv_area_t a = {.y1 = y - 7, .y2 = y + 7};
    if (align == LV_TEXT_ALIGN_CENTER) { a.x1 = x - w / 2; a.x2 = x + w / 2; }
    else if (align == LV_TEXT_ALIGN_RIGHT) { a.x1 = x - w; a.x2 = x; }
    else { a.x1 = x; a.x2 = x + w; }
    lv_draw_label(layer, &d, &a);
}

static void plot_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col, int32_t w)
{
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.p1.x = x1; l.p1.y = y1; l.p2.x = x2; l.p2.y = y2;
    l.color = col;
    l.width = w;
    lv_draw_line(layer, &l);
}

static int32_t jint(const cJSON *o, const char *key, int32_t def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : def;
}

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

// Garante um buffer do canvas com w x h (realoca só se o tamanho mudar).
static bool canvas_size(int32_t w, int32_t h)
{
    if (w < 16 || h < 16 || w * h > PLOT_MAX_PX) {
        return false;
    }
    if (w != s_cw || h != s_ch || !s_cmem) {
        uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
        if (s_cmem && lv_canvas_get_draw_buf(s_canvas) != &s_cbuf) {
            // canvas novo (o antigo já foi apagado): liberta primeiro, para não ter dois buffers grandes
            free(s_cmem);
            s_cmem = NULL;
            s_cw = s_ch = 0;
        }
        uint8_t *mem = heap_caps_malloc(stride * h, MALLOC_CAP_8BIT);
        if (!mem) {
            ESP_LOGE(TAG, "sem memória para canvas %ldx%ld", (long)w, (long)h);
            return false;
        }
        lv_draw_buf_init(&s_cbuf, w, h, LV_COLOR_FORMAT_RGB565, stride, mem, stride * h);
        lv_canvas_set_draw_buf(s_canvas, &s_cbuf);
        free(s_cmem);
        s_cmem = mem;
        s_cw = w;
        s_ch = h;
    } else if (lv_canvas_get_draw_buf(s_canvas) != &s_cbuf) {
        lv_canvas_set_draw_buf(s_canvas, &s_cbuf);
    }
    return true;
}

static void paint_bg(const char *key)
{
    if (!key || !s_bg_ready || strcmp(key, s_bg_key) || s_bg_w != s_cw || s_bg_h != s_ch) {
        lv_canvas_fill_bg(s_canvas, lv_color_hex(C_BG), LV_OPA_COVER);
        return;
    }
    for (int32_t y = 0; y < s_ch; y++) {
        uint16_t *row = (uint16_t *)(s_cbuf.data + y * s_cbuf.header.stride);
        if (s_bg_bpp == 8) {
            const uint8_t *src = s_bg + (size_t)y * s_cw;
            for (int32_t x = 0; x < s_cw; x++) row[x] = s_bg_pal[src[x]];
            continue;
        }
        for (int32_t x = 0; x < s_cw; x++) {
            size_t i = (size_t)y * s_cw + x;
            uint8_t b = s_bg[i / 2];
            row[x] = s_bg_pal[(i & 1) ? (b & 0x0F) : (b >> 4)];
        }
    }
}

// Liberta os ícones (e o dock que os usa) para dar memória ao relógio. O host volta a enviá-los
// quando forem precisos ({"t":"iconsfreed"}). Chamado com o lock do LVGL.
static void icons_free_all(void)
{
    lv_obj_clean(s_dock);
    memset(s_dock_ids, 0, sizeof(s_dock_ids));
    s_dock_on = false;
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    int n = 0;
    for (int i = 0; i < ICON_MAX; i++) {
        if (s_icons[i].data) {
            lv_image_cache_drop(&s_icons[i].dsc);
            free(s_icons[i].data);
            s_icons[i].data = NULL;
            n++;
        }
    }
    s_icon_next = 0;
    emoji_free_all();
    ESP_LOGI(TAG, "%d ícones libertados (livre %u)", n, (unsigned)esp_get_free_heap_size());
    link_send_event("iconsfreed", NULL, NULL, NULL, NULL);
}

// Garante um bloco contíguo de `need` bytes, libertando os ícones se for preciso.
static void make_room(size_t need)
{
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < need + 4096) {
        bool any = false;
        for (int i = 0; i < ICON_MAX && !any; i++) any = s_icons[i].data != NULL;
        if (any) icons_free_all();
    }
}

static void bg_ack(int32_t off, bool ok)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "bgack");
    cJSON_AddNumberToObject(o, "off", off);
    cJSON_AddBoolToObject(o, "ok", ok);
    link_send(o);
    cJSON_Delete(o);
}

// Chamado fora do lock do LVGL (só mexe no buffer). Cada pedaço é confirmado com "bgack";
// a imagem só é usada se todos chegarem em ordem.
static void handle_bg(const cJSON *msg)
{
    int32_t off = jint(msg, "off", 0);
    if (off == 0) {
        s_bg_next = 0;
    }
    if (off == 0) {
        int32_t w = jint(msg, "w", 0), h = jint(msg, "h", 0);
        uint8_t bpp = jint(msg, "bpp", 4) == 8 ? 8 : 4;
        size_t need = bpp == 8 ? (size_t)w * h : ((size_t)w * h + 1) / 2;
        // o relógio lê o fundo num timer do LVGL: a troca do buffer é feita com o lock
        if (!lvgl_port_lock(1000)) {
            bg_ack(off, false);
            return;
        }
        s_bg_ready = false;
        if (w <= 0 || h <= 0 || w * h > PLOT_MAX_PX) {
            lvgl_port_unlock();
            return;
        }
        if (need != s_bg_size) {
            free(s_bg);
            s_bg = NULL;
            s_bg_size = 0;
            if (bpp == 8) make_room(need + (size_t)s_W * s_H * 2);   // fundo + ecrã do relógio
            s_bg = heap_caps_malloc(need, MALLOC_CAP_8BIT);
            s_bg_size = s_bg ? need : 0;
            if (!s_bg) {
                ESP_LOGE(TAG, "sem memória para fundo (%u B, livre %u)", (unsigned)need,
                         (unsigned)esp_get_free_heap_size());
                lvgl_port_unlock();
                bg_ack(off, false);
                return;
            }
        }
        s_bg_w = w;
        s_bg_h = h;
        s_bg_bpp = bpp;
        strlcpy(s_bg_key, jstr(msg, "key", ""), sizeof(s_bg_key));
        memset(s_bg_pal, 0, sizeof(s_bg_pal));
        const cJSON *pal = cJSON_GetObjectItemCaseSensitive(msg, "pal");
        for (int i = 0; i < (bpp == 8 ? 256 : 16); i++) {
            const cJSON *c = cJSON_GetArrayItem(pal, i);
            if (cJSON_IsString(c) && c->valuestring[0] == '#') {
                s_bg_pal[i] = rgb565((uint32_t)strtoul(c->valuestring + 1, NULL, 16));
            }
        }
        lvgl_port_unlock();
    }
    if (!s_bg || off < 0 || (size_t)off != s_bg_next || (size_t)off >= s_bg_size) {
        bg_ack(off, false);
        return;
    }
    const char *d = jstr(msg, "d", "");
    size_t olen = 0;
    if (mbedtls_base64_decode(s_bg + off, s_bg_size - off, &olen, (const unsigned char *)d, strlen(d)) != 0) {
        ESP_LOGW(TAG, "bg: base64 inválido em %ld", (long)off);
        bg_ack(off, false);
        return;
    }
    s_bg_next = off + olen;
    if (jbool(msg, "last", false)) {
        s_bg_ready = s_bg_next == s_bg_size || s_bg_next + 1 == s_bg_size;
    }
    bg_ack(off, true);
}

static void draw_plot(const cJSON *p)
{
    if (!s_canvas || !cJSON_IsObject(p)) {
        return;
    }
    int32_t w = jint(p, "w", 160), h = jint(p, "h", 160);
    if (!canvas_size(w, h)) {
        return;
    }
    const int32_t cx = jint(p, "cx", w / 2), cy = jint(p, "cy", h / 2);
    const int32_t R = jint(p, "R", (w < h ? w : h) / 2 - 3);
    const lv_color_t grid = lv_color_hex(C_CARD_PR), sub = lv_color_hex(C_SUB);
    paint_bg(jstr(p, "bg", NULL));
    draw_tracks(cJSON_GetObjectItemCaseSensitive(p, "tracks"));
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    // anéis (raio em px)
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx;
    a.center.y = cy;
    a.start_angle = 0;
    a.end_angle = 360;
    a.width = 1;
    a.color = jstr(p, "bg", NULL) ? lv_color_hex(0x4A5A6A) : grid;
    const cJSON *rings = cJSON_GetObjectItemCaseSensitive(p, "rings");
    const cJSON *rl = cJSON_GetObjectItemCaseSensitive(p, "rlabels");
    int ri = 0;
    const cJSON *rv;
    cJSON_ArrayForEach(rv, rings) {
        if (cJSON_IsNumber(rv) && rv->valuedouble > 0) {
            int32_t r = (int32_t)rv->valuedouble;
            a.radius = r;
            lv_draw_arc(&layer, &a);
            const cJSON *lab = cJSON_GetArrayItem(rl, ri);
            if (cJSON_IsString(lab) && lab->valuestring[0]) {
                int32_t d = (int32_t)(r * 0.7071f);
                plot_text(&layer, lab->valuestring, cx + d + 1, cy + d - 6, 50, sub, LV_TEXT_ALIGN_LEFT);
            }
        }
        ri++;
    }
    if (jbool(p, "cross", true)) {
        plot_line(&layer, cx, cy - R, cx, cy + R, grid, 1);
        plot_line(&layer, cx - R, cy, cx + R, cy, grid, 1);
    }
    if (jbool(p, "cardinal", false)) {
        plot_text(&layer, "N", cx + 3, cy - R + 8, 12, lv_color_hex(C_ERR), LV_TEXT_ALIGN_LEFT);
        plot_text(&layer, "S", cx + 3, cy + R - 8, 12, sub, LV_TEXT_ALIGN_LEFT);
        plot_text(&layer, "E", cx + R - 1, cy - 8, 12, sub, LV_TEXT_ALIGN_RIGHT);
        plot_text(&layer, "W", cx - R + 1, cy - 8, 16, sub, LV_TEXT_ALIGN_LEFT);
    }
    if (jbool(p, "center", false)) {
        lv_color_t ac = lv_color_hex(C_ACCENT);
        plot_line(&layer, cx - 4, cy, cx + 4, cy, ac, 2);
        plot_line(&layer, cx, cy - 4, cx, cy + 4, ac, 2);
    }

    // pontos: [x, y, cor, rótulo, rumo, cheio] em px
    const cJSON *pts = cJSON_GetObjectItemCaseSensitive(p, "pts");
    int n = 0;
    const cJSON *pt;
    cJSON_ArrayForEach(pt, pts) {
        if (n++ >= PLOT_PTS) break;
        const cJSON *jx = cJSON_GetArrayItem(pt, 0), *jy = cJSON_GetArrayItem(pt, 1);
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) continue;
        const cJSON *jc = cJSON_GetArrayItem(pt, 2), *jl = cJSON_GetArrayItem(pt, 3);
        const cJSON *jh = cJSON_GetArrayItem(pt, 4), *jf = cJSON_GetArrayItem(pt, 5);
        int32_t px = (int32_t)jx->valuedouble, py = (int32_t)jy->valuedouble;
        lv_color_t col = parse_color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_TEXT);
        if (cJSON_IsNumber(jh)) {
            float rad = (float)jh->valuedouble * (float)M_PI / 180.0f;
            plot_line(&layer, px, py, px + (int32_t)lroundf(10 * sinf(rad)), py - (int32_t)lroundf(10 * cosf(rad)),
                      col, 2);
        }
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.radius = LV_RADIUS_CIRCLE;
        bool fill = !cJSON_IsNumber(jf) || jf->valueint;
        rd.bg_color = col;
        rd.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
        rd.border_color = col;
        rd.border_width = fill ? 0 : 1;
        lv_area_t ar = {px - 3, py - 3, px + 3, py + 3};
        lv_draw_rect(&layer, &rd, &ar);
        if (cJSON_IsString(jl) && jl->valuestring[0]) {
            bool left = px > w - 45;   // rótulo à esquerda junto à borda direita
            plot_text(&layer, jl->valuestring, left ? px - 5 : px + 5, py - 7, 50, col,
                      left ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
        }
    }

    const char *msg = jstr(p, "msg", NULL);
    if (msg && msg[0]) {
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_hex(C_BAR);
        bg.bg_opa = LV_OPA_80;
        bg.radius = 6;
        lv_area_t ba = {8, cy - 18, w - 9, cy + 18};
        lv_draw_rect(&layer, &bg, &ba);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = msg;
        d.text_local = 1;
        d.font = &dash_font_12;
        d.color = lv_color_hex(C_WARN);
        d.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t ta = {12, cy - 16, w - 13, cy + 17};
        lv_draw_label(&layer, &d, &ta);
    }
    lv_canvas_finish_layer(s_canvas, &layer);
}

// Trajetos: escritos direto no buffer RGB565 (Bresenham), depois do fundo e antes da layer.
static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static inline void px_put(int32_t x, int32_t y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < s_cw && y < s_ch) {
        ((uint16_t *)(s_cbuf.data + y * s_cbuf.header.stride))[x] = c;
    }
}

static void raw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c, int wdt)
{
    // segmentos totalmente fora do canvas não são percorridos
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= s_cw && x1 >= s_cw) || (y0 >= s_ch && y1 >= s_ch)) {
        return;
    }
    int32_t dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    bool steep = -dy > dx;
    for (int guard = 0; guard < 8192; guard++) {
        px_put(x0, y0, c);
        if (wdt > 1) px_put(x0 + (steep ? 1 : 0), y0 + (steep ? 0 : 1), c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_tracks(const cJSON *tracks)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, tracks) {
        const cJSON *jc = cJSON_GetArrayItem(t, 0), *jw = cJSON_GetArrayItem(t, 1), *jd = cJSON_GetArrayItem(t, 2);
        if (!cJSON_IsString(jd)) continue;
        lv_color_t col = parse_color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_SUB);
        uint16_t c = lv_color_to_u16(col);
        int wdt = cJSON_IsNumber(jw) ? jw->valueint : 1;
        const char *d = jd->valuestring;
        size_t n = strlen(d) / 4;
        int32_t px = 0, py = 0;
        for (size_t i = 0; i < n; i++, d += 4) {
            int a = b64v(d[0]), b = b64v(d[1]), e = b64v(d[2]), f = b64v(d[3]);
            if (a < 0 || b < 0 || e < 0 || f < 0) break;
            int32_t x = (a << 6 | b) - 1024, y = (e << 6 | f) - 1024;
            if (i) raw_line(px, py, x, y, c, wdt);
            px = x;
            py = y;
        }
    }
}

static void ctrl_click_cb(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < CTRL_MAX && s_ctrl_ids[i][0]) {
        link_send_event("btn", "id", s_screen_id, "btn", s_ctrl_ids[i]);
    }
}

// Botões sobre o canvas. Só são refeitos quando a lista muda (o mapa atualiza a cada 2 s).
static void build_ctrl(const cJSON *ctrl)
{
    if (!s_plot_wrap) return;
    char *sig = cJSON_IsArray(ctrl) ? cJSON_PrintUnformatted(ctrl) : NULL;
    const char *now = sig ? sig : "";
    if (!strncmp(now, s_ctrl_sig, sizeof(s_ctrl_sig) - 1) && strlen(now) < sizeof(s_ctrl_sig)) {
        cJSON_free(sig);
        return;
    }
    strlcpy(s_ctrl_sig, now, sizeof(s_ctrl_sig));
    cJSON_free(sig);
    // apaga os botões antigos (o filho 0 é o canvas)
    while (lv_obj_get_child_count(s_plot_wrap) > 1) {
        lv_obj_delete(lv_obj_get_child(s_plot_wrap, 1));
    }
    memset(s_ctrl_ids, 0, sizeof(s_ctrl_ids));
    int i = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrl) {
        if (i >= CTRL_MAX) break;
        const cJSON *jid = cJSON_GetArrayItem(c, 0), *jpos = cJSON_GetArrayItem(c, 1), *jl = cJSON_GetArrayItem(c, 2);
        if (!cJSON_IsString(jid) || !cJSON_IsString(jpos)) continue;
        const char *pos = jpos->valuestring;
        const char *lbl = cJSON_IsString(jl) && jl->valuestring[0] ? jl->valuestring : NULL;
        lv_align_t al = LV_ALIGN_TOP_LEFT;
        const char *sym = "";
        if (!strcmp(pos, "tr")) al = LV_ALIGN_TOP_RIGHT;
        else if (!strcmp(pos, "bl")) al = LV_ALIGN_BOTTOM_LEFT;
        else if (!strcmp(pos, "br")) al = LV_ALIGN_BOTTOM_RIGHT;
        else if (!strcmp(pos, "l")) { al = LV_ALIGN_LEFT_MID; sym = LV_SYMBOL_LEFT; }
        else if (!strcmp(pos, "r")) { al = LV_ALIGN_RIGHT_MID; sym = LV_SYMBOL_RIGHT; }
        else if (!strcmp(pos, "t")) { al = LV_ALIGN_TOP_MID; sym = LV_SYMBOL_UP; }
        else if (!strcmp(pos, "b")) { al = LV_ALIGN_BOTTOM_MID; sym = LV_SYMBOL_DOWN; }
        strlcpy(s_ctrl_ids[i], jid->valuestring, ID_LEN);
        lv_obj_t *b = lv_obj_create(s_plot_wrap);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 30, 26);
        lv_obj_align(b, al, 0, 0);
        lv_obj_set_style_radius(b, 7, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_BAR), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_CARD_PR), 0);
        lv_obj_set_ext_click_area(b, 4);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);   // tocar numa seta não é swipe
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, ctrl_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mk_label(b, &dash_font_16, C_TEXT, lbl ? lbl : sym);
        lv_obj_center(l);
        i++;
    }
}

// ---------- páginas (pontos) ----------

// ---------- relógio ----------

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool clock_tm(struct tm *tm)
{
    if (!s_has_time) return false;
    time_t t = (time_t)((esp_timer_get_time() + s_epoch_off) / 1000000);
    gmtime_r(&t, tm);   // o epoch já vem em hora local
    return true;
}

static void clock_draw(void)
{
    if (!s_clock || !s_canvas || !s_cmem) return;
    struct tm tm = {0};
    s_cs.has_time = clock_tm(&tm);
    uint32_t ms = now_ms();
    s_cs.show_bright = (int32_t)(s_bright_until - ms) > 0;
    s_cs.show_dots = (int32_t)(s_dots_until - ms) > 0;
    s_cs.show_exit = (int32_t)(s_exit_until - ms) > 0;
    s_cs.bright = s_bl;
    if (s_cs.face == 0) {
        bool ok = s_bg_ready && !strncmp(s_bg_key, "clk", 3) && s_bg_w == s_cw && s_bg_h == s_ch;
        if (ok) {
            paint_bg(s_bg_key);
            s_clock_drawn_bg = true;
        } else if (s_clock_drawn_bg && !s_bg_ready) {
            return;   // fundo novo a chegar (~1 s): mantém a última imagem
        } else {
            lv_canvas_fill_bg(s_canvas, lv_color_hex(0x101830), LV_OPA_COVER);
        }
    }
    clockface_draw((uint16_t *)s_cbuf.data, (int)(s_cbuf.header.stride / 2), s_cw, s_ch, &s_cs, &tm, ms);
    lv_obj_invalidate(s_canvas);
    s_clock_last_sec = s_cs.has_time ? tm.tm_sec : -1;
}

static void clock_timer_cb(lv_timer_t *t)
{
    if (!s_clock) return;
    struct tm tm;
    bool ht = clock_tm(&tm);
    uint32_t ms = now_ms();
    bool ovl = (int32_t)(s_bright_until - ms) > 0 || (int32_t)(s_dots_until - ms) > 0 ||
               (int32_t)(s_exit_until - ms) > 0;
    bool bg_new = s_cs.face == 0 && s_bg_ready && !s_clock_drawn_bg;
    if (clockface_animated(&s_cs) || (ht && tm.tm_sec != s_clock_last_sec) || ovl || ovl != s_ovl_was || bg_new) {
        clock_draw();
    }
    s_ovl_was = ovl;
    if (!s_dimmed && s_dim_ms && lv_display_get_inactive_time(NULL) > s_dim_ms && s_dim_bl < s_bl) {
        s_dimmed = true;
        board_set_backlight(s_dim_bl);
    }
}

static void clock_gesture(lv_dir_t d);

// Gestos do relógio medidos aqui (do toque ao soltar), sem o detetor do LVGL: na altura de 172 px
// um swipe vertical raramente chegava ao limite dele e era tratado como toque (saía do relógio).
static void clock_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        s_press_pt = p;
        s_pressing = true;
        if (s_dimmed) {   // escurecido: o toque repõe o brilho (e segue como um toque normal)
            s_dimmed = false;
            board_set_backlight(s_bl);
        }
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_pressing) return;
    s_pressing = false;
    int32_t dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int32_t th = 24;
    if (ax >= th && ax > ay) {
        clock_gesture(dx < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT);
    } else if (ay >= th) {
        clock_gesture(dy < 0 ? LV_DIR_TOP : LV_DIR_BOTTOM);
    } else {
        uint32_t ms = now_ms();
        if ((int32_t)(s_exit_until - ms) > 0) {   // 2.º toque dentro do aviso: sai
            s_exit_until = ms;
            go_back();
        } else {
            s_exit_until = ms + 2500;
            clock_draw();
        }
    }
}

static void clock_apply(const cJSON *c)
{
    if (!cJSON_IsObject(c)) return;
    s_cs.face = jint(c, "face", s_cs.face) ? 1 : 0;
    s_cs.pal = jint(c, "pal", s_cs.pal);
    s_cs.night = jbool(c, "night", false);
    int dim = jint(c, "dim", -1), dim_bl = jint(c, "dim_bl", -1);
    if (dim >= 0) s_dim_ms = (uint32_t)dim * 1000;
    if (dim_bl >= 0 && dim_bl <= 100) s_dim_bl = (uint8_t)dim_bl;
    const char *an = jstr(c, "anim", "");
    s_cs.anim = !strcmp(an, "rain") ? CLK_ANIM_RAIN : !strcmp(an, "storm") ? CLK_ANIM_STORM
              : !strcmp(an, "snow") ? CLK_ANIM_SNOW : CLK_ANIM_NONE;
    strlcpy(s_clock_bg, jstr(c, "bg", ""), sizeof(s_clock_bg));
    const cJSON *wx = cJSON_GetObjectItemCaseSensitive(c, "wx");
    s_cs.has_wx = cJSON_IsObject(wx);
    if (s_cs.has_wx) {
        s_cs.temp = jint(wx, "t", 0);
        s_cs.tmax = jint(wx, "max", 0);
        s_cs.tmin = jint(wx, "min", 0);
        s_cs.rain = jint(wx, "rain", 0);
        s_cs.wind = jint(wx, "wind", 0);
    }
    // ícone 16x16: "pal" e "role" por cor (a cor i é o índice i+1; 0 = transparente)
    const cJSON *ic = cJSON_GetObjectItemCaseSensitive(c, "icon");
    const char *d = jstr(ic, "d", "");
    s_cs.has_icon = strlen(d) == 256;
    if (s_cs.has_icon) {
        const cJSON *pal = cJSON_GetObjectItemCaseSensitive(ic, "pal");
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(ic, "role");
        for (int i = 0; i < 7; i++) {
            const cJSON *p = cJSON_GetArrayItem(pal, i), *r = cJSON_GetArrayItem(role, i);
            s_cs.icon_pal[i + 1] = (cJSON_IsString(p) && p->valuestring[0] == '#')
                                       ? rgb565((uint32_t)strtoul(p->valuestring + 1, NULL, 16)) : 0xFFFF;
            s_cs.icon_role[i + 1] = cJSON_IsNumber(r) ? (uint8_t)r->valueint : 1;
        }
        for (int i = 0; i < 256; i++) s_cs.icon[i] = (d[i] >= '0' && d[i] <= '7') ? (uint8_t)(d[i] - '0') : 0;
    }
}

static void clock_enter(const cJSON *msg)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(msg, "clock");
    if (!s_clock) {
        s_clock = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_clock);
        lv_obj_set_pos(s_clock, 0, 0);
        lv_obj_set_size(s_clock, s_W, s_H);
        lv_obj_set_style_bg_opa(s_clock, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_clock, lv_color_hex(0x000000), 0);
        lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_GESTURE_BUBBLE);   // o gesto é tratado em clock_press_cb
        lv_obj_add_event_cb(s_clock, clock_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_clock, clock_press_cb, LV_EVENT_RELEASED, NULL);
        s_exit_until = now_ms();
        s_pressing = false;
        s_dimmed = false;
        lv_display_trigger_activity(NULL);
        s_canvas = lv_canvas_create(s_clock);
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        if (s_cmem && (s_cw != s_W || s_ch != s_H)) {   // buffer de um plot antigo: liberta antes de medir
            free(s_cmem);
            s_cmem = NULL;
            s_cw = s_ch = 0;
        }
        if (!s_cmem) make_room((size_t)lv_draw_buf_width_to_stride(s_W, LV_COLOR_FORMAT_RGB565) * s_H);
        if (!canvas_size(s_W, s_H)) {
            lv_obj_delete(s_clock);
            s_clock = NULL;
            s_canvas = NULL;
            show_toast("Sem memória para o relógio", "err");
            return;
        }
        s_bl_saved = s_bl;
        int bl = jint(c, "bl", -1);
        if (bl >= 10 && bl <= 100) {
            s_bl = (uint8_t)bl;
            board_set_backlight(s_bl);
        }
        s_clock_drawn_bg = false;
        s_clock_timer = lv_timer_create(clock_timer_cb, 125, NULL);
        ESP_LOGI(TAG, "relógio: heap livre %u", (unsigned)esp_get_free_heap_size());
    }
    s_canvas = lv_obj_get_child(s_clock, 0);   // render_screen limpou o ponteiro
    lv_obj_move_foreground(s_clock);
    clock_apply(c);
    s_clock_last_sec = -1;
    clock_draw();
}

static void clock_exit(void)
{
    if (!s_clock) return;
    if (s_clock_timer) lv_timer_delete(s_clock_timer);
    s_clock_timer = NULL;
    lv_obj_delete(s_clock);
    s_clock = NULL;
    s_canvas = NULL;
    // devolve a memória (o host volta a enviar ícones e fundos quando precisar)
    free(s_cmem);
    s_cmem = NULL;
    s_cw = s_ch = 0;
    free(s_bg);
    s_bg = NULL;
    s_bg_size = 0;
    s_bg_ready = false;
    s_bg_key[0] = '\0';
    s_bl = s_bl_saved;
    board_set_backlight(s_bl);
}

// Gestos no relógio: lateral troca o mostrador, vertical muda o brilho.
static void clock_gesture(lv_dir_t d)
{
    char v[8];
    uint32_t ms = now_ms();
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
        s_cs.face ^= 1;
        s_dots_until = ms + 1500;
        snprintf(v, sizeof(v), "%d", s_cs.face);
        link_send_event("clock", "face", v, NULL, NULL);
    } else if (d == LV_DIR_TOP || d == LV_DIR_BOTTOM) {
        int bl = s_bl + (d == LV_DIR_TOP ? 10 : -10);
        bl = bl < 10 ? 10 : bl > 100 ? 100 : bl;
        s_bl = (uint8_t)bl;
        board_set_backlight(s_bl);
        s_bright_until = ms + 1500;
        snprintf(v, sizeof(v), "%d", bl);
        link_send_event("clock", "bl", v, NULL, NULL);
    }
    clock_draw();
}

static void set_pager(const cJSON *msg)
{
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(msg, "pages");
    const cJSON *ji = cJSON_GetObjectItemCaseSensitive(msg, "page");
    int pages = cJSON_IsNumber(jp) ? jp->valueint : 1;
    int page = cJSON_IsNumber(ji) ? ji->valueint : 0;
    if (pages == s_pages && page == s_page) {
        return;
    }
    s_pages = pages;
    s_page = page;
    lv_obj_clean(s_pager);
    if (pages <= 1) {
        lv_obj_add_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    for (int i = 0; i < pages && i < 8; i++) {
        lv_obj_t *d = plain_box(s_pager);
        lv_obj_set_size(d, i == page ? 10 : 5, 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(i == page ? C_ACCENT : C_CARD_PR), 0);
    }
    lv_obj_remove_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !s_has_screen) {
        return;
    }
    lv_dir_t d = lv_indev_get_gesture_dir(indev);
    if (s_tpl == TPL_CLOCK && s_clock) {
        return;   // no relógio os gestos são medidos em clock_press_cb
    }
    if ((d != LV_DIR_LEFT && d != LV_DIR_RIGHT) || s_hgrid) {   // no scroll lateral o gesto é do scroll
        return;
    }
    lv_indev_wait_release(indev);   // não conta como toque no item
    link_send_event("swipe", "id", s_screen_id, "dir", d == LV_DIR_LEFT ? "left" : "right");
}

static tpl_t parse_tpl(const char *s)
{
    if (!strcmp(s, "menu")) return TPL_MENU;
    if (!strcmp(s, "list")) return TPL_LIST;
    if (!strcmp(s, "kv")) return TPL_KV;
    if (!strcmp(s, "log")) return TPL_LOG;
    if (!strcmp(s, "text")) return TPL_TEXT;
    if (!strcmp(s, "plot")) return TPL_PLOT;
    if (!strcmp(s, "clock")) return TPL_CLOCK;
    return TPL_MENU;
}

// Constrói (rebuild=true) ou atualiza no lugar o corpo a partir de msg.
static void apply_body(const cJSON *msg, bool rebuild)
{
    const cJSON *arr = NULL;
    if (s_tpl == TPL_CLOCK) {
        if (s_clock) {
            clock_apply(cJSON_GetObjectItemCaseSensitive(msg, "clock"));
            clock_draw();
        }
        return;
    }
    switch (s_tpl) {
    case TPL_MENU:
    case TPL_LIST: arr = cJSON_GetObjectItemCaseSensitive(msg, "items"); break;
    case TPL_KV:
    case TPL_PLOT: arr = cJSON_GetObjectItemCaseSensitive(msg, "rows"); break;
    default: break;
    }

    if (s_tpl == TPL_PLOT) {
        if (rebuild) {
            lv_obj_clean(s_body);
            const cJSON *pl = cJSON_GetObjectItemCaseSensitive(msg, "plot");
            int32_t pw = jint(pl, "w", 160);
            s_plot_wrap = plain_box(s_body);
            lv_obj_remove_flag(s_plot_wrap, LV_OBJ_FLAG_SCROLLABLE);
            s_canvas = lv_canvas_create(s_plot_wrap);
            s_ctrl_sig[0] = '\0';
            s_list = plain_box(s_body);
            int32_t rest = (s_W - 12) - pw - 6;
            lv_obj_set_size(s_list, rest >= 110 ? rest : LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(s_list, 6, 0);
        }
        const cJSON *pl = cJSON_GetObjectItemCaseSensitive(msg, "plot");
        if (s_plot_wrap) lv_obj_set_size(s_plot_wrap, jint(pl, "w", 160), jint(pl, "h", 160));
        draw_plot(pl);
        build_ctrl(cJSON_GetObjectItemCaseSensitive(pl, "ctrl"));
    }

    if (arr && cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > MAX_ITEMS) n = MAX_ITEMS;
        if (!rebuild && n == s_n) {
            for (int i = 0; i < n; i++) {
                const cJSON *it = cJSON_GetArrayItem(arr, i);
                if (IS_KV(s_tpl)) update_kv_row(i, it);
                else {
                    strlcpy(s_item_ids[i], jstr(it, "id", ""), ID_LEN);
                    utf8cpy(s_item_confirm[i], jstr(it, "confirm", ""), sizeof(s_item_confirm[i]));
                    if (s_tpl == TPL_MENU) update_menu_item(i, it);
                    else update_list_item(i, it);
                }
            }
        } else {
            int32_t sy = lv_obj_get_scroll_y(s_content), sx = lv_obj_get_scroll_x(s_content);
            lv_obj_clean(s_list);
            memset(s_badge, 0, sizeof(s_badge));
            s_n = 0;
            for (int i = 0; i < n; i++) {
                const cJSON *it = cJSON_GetArrayItem(arr, i);
                if (IS_KV(s_tpl)) build_kv_row(i, it);
                else {
                    strlcpy(s_item_ids[i], jstr(it, "id", ""), ID_LEN);
                    utf8cpy(s_item_confirm[i], jstr(it, "confirm", ""), sizeof(s_item_confirm[i]));
                    if (s_tpl == TPL_MENU && s_grid) build_tile(i, it);
                    else if (s_tpl == TPL_MENU) build_menu_item(i, it);
                    else build_list_item(i, it);
                }
                s_n = i + 1;
            }
            if (n == 0 && s_tpl != TPL_MENU && s_tpl != TPL_PLOT) {
                mk_label(s_list, &dash_font_12, C_SUB, jstr(msg, "empty", "Sem dados"));
            }
            if (!rebuild) {
                lv_obj_update_layout(s_content);
                lv_obj_scroll_to(s_content, sx, sy, LV_ANIM_OFF);
            }
        }
    }

    if (s_tpl == TPL_LOG) {
        if (rebuild) {
            lv_obj_clean(s_body);
            s_log_lbl = mk_label(s_body, &dash_font_12, C_TEXT, "");
            lv_obj_set_width(s_log_lbl, LV_PCT(100));
            lv_label_set_long_mode(s_log_lbl, LV_LABEL_LONG_WRAP);
        }
        const cJSON *lines = cJSON_GetObjectItemCaseSensitive(msg, "lines");
        const cJSON *app = cJSON_GetObjectItemCaseSensitive(msg, "append");
        if (cJSON_IsArray(lines)) log_set_lines(lines, false);
        if (cJSON_IsArray(app)) log_set_lines(app, true);
    }

    if (s_tpl == TPL_TEXT) {
        const char *txt = jstr(msg, "text", NULL);
        if (rebuild) {
            lv_obj_clean(s_body);
            s_log_lbl = mk_label(s_body, &dash_font_14, C_TEXT, txt ? txt : "");
            lv_obj_set_width(s_log_lbl, LV_PCT(100));
            lv_label_set_long_mode(s_log_lbl, LV_LABEL_LONG_WRAP);
        } else if (txt) {
            lv_label_set_text(s_log_lbl, txt);
        }
    }

    const cJSON *btns = cJSON_GetObjectItemCaseSensitive(msg, "buttons");
    if (cJSON_IsArray(btns)) build_buttons(btns);
    else if (rebuild) build_buttons(NULL);
}

static void layout_content(bool dock)
{
    bool show = dock && s_dock_on;
    if (show) lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_content, s_H - BAR_H - (show ? DOCK_H : 0));
    s_content_h = s_H - BAR_H - (show ? DOCK_H : 0) - 12;
}

static void dock_click_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < DOCK_MAX && s_dock_ids[idx][0]) {
        link_send_event("dock", "item", s_dock_ids[idx], NULL, NULL);
    }
}

static void build_dock(const cJSON *items)
{
    lv_obj_clean(s_dock);
    memset(s_dock_ids, 0, sizeof(s_dock_ids));
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (n >= DOCK_MAX) break;
        strlcpy(s_dock_ids[n], jstr(it, "id", ""), ID_LEN);
        lv_obj_t *b = plain_box(s_dock);
        lv_obj_set_size(b, LV_SIZE_CONTENT, DOCK_H - 4);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(b, dock_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        const lv_image_dsc_t *img = icon_find(jstr(it, "img", NULL));
        if (img) {
            lv_obj_t *im = lv_image_create(b);
            lv_image_set_src(im, img);
            lv_image_set_scale(im, 160);           // 32 px -> 20 px
            lv_obj_set_size(im, 20, 20);
            lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);
        } else {
            lv_obj_t *ic = mk_label(b, &dash_font_16, C_ACCENT, icon_symbol(jstr(it, "icon", NULL)));
            lv_obj_set_style_text_color(ic, parse_color(jstr(it, "color", NULL), C_ACCENT), 0);
        }
        mk_label(b, &dash_font_12, C_SUB, jstr(it, "label", ""));
        n++;
    }
    s_dock_on = n > 0;
}

static void render_screen(const cJSON *msg)
{
    strlcpy(s_screen_id, jstr(msg, "id", ""), ID_LEN);
    s_tpl = parse_tpl(jstr(msg, "tpl", "menu"));
    if (s_tpl != TPL_CLOCK) clock_exit();
    s_has_screen = true;
    s_n = 0;
    s_log_lbl = NULL;
    s_canvas = NULL;
    s_plot_wrap = NULL;
    s_list = s_body;
    memset(s_badge, 0, sizeof(s_badge));
    set_pager(msg);
    lv_label_set_text(s_title, jstr(msg, "title", "DashDeck"));
    if (jbool(msg, "back", false)) lv_obj_remove_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_body);
    if (s_tpl == TPL_CLOCK) {
        build_buttons(NULL);
        clock_enter(msg);
        return;
    }
    s_grid =s_tpl == TPL_MENU && !strcmp(jstr(msg, "layout", "cards"), "grid");
    s_hgrid = s_grid && s_land;
    s_dock_want = jbool(msg, "dock", true);
    layout_content(s_dock_want);
    bool wrap = s_tpl == TPL_PLOT || (s_tpl == TPL_MENU && (s_land || s_grid));
    if (s_hgrid) {
        // uma fileira de boxes; o conteúdo rola para os lados
        lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(s_body, LV_SIZE_CONTENT);
        lv_obj_set_scroll_dir(s_content, LV_DIR_HOR);
        lv_obj_set_scroll_snap_x(s_content, LV_SCROLL_SNAP_NONE);
    } else {
        lv_obj_set_flex_flow(s_body, wrap ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(s_body, LV_PCT(100));
        lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    }
    lv_obj_scroll_to(s_content, 0, 0, LV_ANIM_OFF);
    apply_body(msg, true);
}

static void show_waiting(void)
{
    lv_label_set_text(s_title, "DashDeck");
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_body);
    build_buttons(NULL);
    s_n = 0;
    s_tpl = TPL_NONE;
    s_canvas = NULL;
    s_plot_wrap = NULL;
    s_list = s_body;
    s_has_screen = false;
    set_pager(NULL);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *box = plain_box(s_body);
    lv_obj_set_size(box, LV_PCT(100), s_H - BAR_H - 12);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_t *sp = lv_spinner_create(box);
    lv_obj_set_size(sp, 48, 48);
    lv_obj_set_style_arc_color(sp, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_MAIN);
    lv_obj_t *l = mk_label(box, &dash_font_14, C_TEXT, "Aguardando uConsole...");
    (void)l;
    mk_label(box, &dash_font_12, C_SUB, "inicie o dashd no host");
    mk_label(box, &dash_font_12, C_SUB, "fw " DASHDECK_FW_VERSION);
}

// ---------- timers ----------

static void send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "hello");
    cJSON_AddStringToObject(o, "fw", DASHDECK_FW_VERSION);
    cJSON_AddNumberToObject(o, "w", s_W);
    cJSON_AddNumberToObject(o, "h", s_H);
    cJSON_AddNumberToObject(o, "rot", s_rot);
    cJSON_AddNumberToObject(o, "heap", (double)esp_get_free_heap_size());
    link_send(o);
    cJSON_Delete(o);
}

static void wake_up(void)
{
    if (!s_asleep) return;
    s_asleep = false;
    board_set_backlight(s_bl);
    if (s_shield) {
        lv_obj_delete_async(s_shield);
        s_shield = NULL;
    }
    lv_display_trigger_activity(NULL);
    ESP_LOGI(TAG, "ecrã ligado");
}

static void shield_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);   // este toque só acorda
    wake_up();
}

static void sleep_enter(void)
{
    s_asleep = true;
    board_set_backlight(0);
    s_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_shield);
    lv_obj_set_size(s_shield, s_W, s_H);
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_PRESSED, NULL);
    ESP_LOGI(TAG, "ecrã apagado (inativo na tela inicial)");
}

// ---------- animação de arranque ----------
// Ao ligar o ESP, a animação ("Pixels" ou "Nascer do sol", ao acaso) é a tela de carregamento: fica por
// cima de tudo, com uma barra de progresso, até o host mandar a primeira tela (e pelo menos 3,5 s).
// Buffer próprio, libertado no fim. Sem resposta em 10 s mostra "A AGUARDAR O UCONSOLE".
static lv_obj_t *s_boot, *s_boot_cv;
static lv_draw_buf_t s_boot_buf;
static uint8_t *s_boot_mem;
static lv_timer_t *s_boot_timer;
static int64_t s_boot_t0;
static float s_boot_done;          // instante (s) em que o host respondeu e a barra fecha; 0 = ainda não

static void boot_end(void)
{
    if (s_boot_timer) lv_timer_delete(s_boot_timer);
    s_boot_timer = NULL;
    if (s_boot) lv_obj_delete(s_boot);
    s_boot = s_boot_cv = NULL;
    free(s_boot_mem);
    s_boot_mem = NULL;
}

static float boot_progress(float el)
{
    // 0 a 70% durante a animação; depois aproxima-se devagar de 95% enquanto o host não responde
    float p = el < BOOTANIM_SECONDS ? 0.7f * el / BOOTANIM_SECONDS
                                    : 0.7f + 0.25f * (1 - expf(-(el - BOOTANIM_SECONDS) / 4));
    if (s_boot_done > 0) p += (1 - p) * fminf(1, (el - s_boot_done) / 0.3f);
    return p;
}

static void boot_timer_cb(lv_timer_t *t)
{
    float el = (float)(esp_timer_get_time() - s_boot_t0) / 1e6f;
    if (s_boot_done == 0 && s_has_screen && el >= BOOTANIM_SECONDS) s_boot_done = el;
    if (s_boot_done > 0 && el > s_boot_done + 0.35f) {
        boot_end();
        return;
    }
    bootanim_draw((uint16_t *)s_boot_buf.data, (int)(s_boot_buf.header.stride / 2), el, boot_progress(el),
                  s_boot_done == 0 && el > 10);
    lv_obj_invalidate(s_boot_cv);
}

static void boot_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();   // o toque não passa para a tela por baixo
    if (indev) lv_indev_wait_release(indev);
}

static bool boot_start(void)
{
    uint32_t stride = lv_draw_buf_width_to_stride(s_W, LV_COLOR_FORMAT_RGB565);
    s_boot_mem = heap_caps_malloc(stride * s_H, MALLOC_CAP_8BIT);
    if (!s_boot_mem) return false;
    lv_draw_buf_init(&s_boot_buf, s_W, s_H, LV_COLOR_FORMAT_RGB565, stride, s_boot_mem, stride * s_H);
    s_boot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_boot);
    lv_obj_set_size(s_boot, s_W, s_H);
    lv_obj_add_flag(s_boot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_boot, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_boot, boot_press_cb, LV_EVENT_PRESSED, NULL);
    s_boot_cv = lv_canvas_create(s_boot);
    lv_obj_remove_flag(s_boot_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_draw_buf(s_boot_cv, &s_boot_buf);
    // Sem Wi-Fi/BT ligados, o esp_random() repete a mesma sequência em cada arranque (saía sempre a
    // mesma animação). O ruído do ADC do arranque dá entropia verdadeira só para esta escolha.
    bootloader_random_enable();
    int variant = (int)(esp_random() & 1);
    bootloader_random_disable();
    bootanim_start(variant, s_W, s_H);
    bootanim_draw((uint16_t *)s_boot_buf.data, (int)(stride / 2), 0, 0, false);
    s_boot_t0 = esp_timer_get_time();
    s_boot_done = 0;
    s_boot_timer = lv_timer_create(boot_timer_cb, 50, NULL);
    ESP_LOGI(TAG, "arranque: animação %s", variant ? "nascer do sol" : "pixels");
    return true;
}

// ---------- aviso de mensagem direta ----------
// {"t":"notify","from":"NOME","text":".."}: por cima de qualquer tela (também do relógio), acende o
// ecrã, escurece o fundo, anima "Ondas LoRa" ao centro e mostra uma faixa com o texto. Um toque envia
// {"t":"notify","act":"open"} (o host abre a mensagem) e fecha o aviso.
static lv_obj_t *s_ntf, *s_ntf_cv, *s_ntf_bar;
static lv_draw_buf_t s_ntf_buf;
static uint8_t *s_ntf_mem;
static lv_timer_t *s_ntf_timer;
static int64_t s_ntf_t0;
static bool s_ntf_close;

static void ntf_end(void)
{
    if (s_ntf_timer) lv_timer_delete(s_ntf_timer);
    s_ntf_timer = NULL;
    if (s_ntf) lv_obj_delete(s_ntf);
    s_ntf = s_ntf_cv = s_ntf_bar = NULL;
    free(s_ntf_mem);
    s_ntf_mem = NULL;
}

static void ntf_timer_cb(lv_timer_t *t)
{
    float el = (float)(esp_timer_get_time() - s_ntf_t0) / 1e6f;
    if (s_ntf_close || el > NOTIFY_TOTAL) {
        ntf_end();
        return;
    }
    float fade = el < 0.3f ? el / 0.3f : el > NOTIFY_TOTAL - 0.3f ? (NOTIFY_TOTAL - el) / 0.3f : 1;
    lv_obj_set_style_bg_opa(s_ntf, (lv_opa_t)(150 * fade), 0);
    if (s_ntf_cv) {
        notify_draw(s_ntf_mem, el);
        lv_obj_invalidate(s_ntf_cv);
    }
    if (el > NOTIFY_ANIM) {   // a faixa sobe de baixo
        float p = (el - NOTIFY_ANIM) / 0.3f;
        p = p > 1 ? 1 : p;
        p = 1 - (1 - p) * (1 - p) * (1 - p);
        lv_obj_set_y(s_ntf_bar, s_H - (int32_t)(lv_obj_get_height(s_ntf_bar) * p));
    }
}

static void ntf_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);   // o toque não passa para a tela por baixo
    link_send_event("notify", "act", "open", NULL, NULL);
    s_ntf_close = true;
}

static void ntf_show(const cJSON *msg)
{
    ntf_end();
    if (s_asleep) wake_up();
    if (s_dimmed) {   // relógio escurecido
        s_dimmed = false;
        board_set_backlight(s_bl);
    }
    lv_display_trigger_activity(NULL);
    s_ntf = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ntf);
    lv_obj_set_size(s_ntf, s_W, s_H);
    lv_obj_set_style_bg_color(s_ntf, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ntf, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_ntf, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ntf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ntf, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_ntf, ntf_press_cb, LV_EVENT_PRESSED, NULL);
    size_t need = (size_t)NOTIFY_W * NOTIFY_H * 3;
    s_ntf_mem = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    if (!s_ntf_mem) {
        make_room(need);
        s_ntf_mem = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    }
    if (s_ntf_mem) {   // sem memória fica só a faixa
        lv_draw_buf_init(&s_ntf_buf, NOTIFY_W, NOTIFY_H, LV_COLOR_FORMAT_RGB565A8, NOTIFY_W * 2, s_ntf_mem, need);
        notify_start(jstr(msg, "from", ""));
        notify_draw(s_ntf_mem, 0);
        s_ntf_cv = lv_canvas_create(s_ntf);
        lv_obj_remove_flag(s_ntf_cv, LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_set_draw_buf(s_ntf_cv, &s_ntf_buf);
        lv_obj_align(s_ntf_cv, LV_ALIGN_CENTER, 0, s_land ? -20 : -30);
    }
    s_ntf_bar = plain_box(s_ntf);
    lv_obj_set_size(s_ntf_bar, s_W, 42);
    lv_obj_set_pos(s_ntf_bar, 0, s_H);
    lv_obj_set_style_bg_opa(s_ntf_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ntf_bar, lv_color_hex(0x0E3B1C), 0);
    lv_obj_set_style_border_side(s_ntf_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ntf_bar, 1, 0);
    lv_obj_set_style_border_color(s_ntf_bar, lv_color_hex(0x67EA94), 0);
    lv_obj_set_style_pad_hor(s_ntf_bar, 8, 0);
    lv_obj_set_style_pad_ver(s_ntf_bar, 4, 0);
    lv_obj_set_flex_flow(s_ntf_bar, LV_FLEX_FLOW_COLUMN);
    char head[64];
    snprintf(head, sizeof(head), "Mensagem de %s", jstr(msg, "from", "?"));
    lv_obj_t *l1 = mk_label(s_ntf_bar, &dash_font_14, 0x67EA94, head);
    lv_obj_set_width(l1, LV_PCT(100));
    lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
    lv_obj_t *l2 = mk_label(s_ntf_bar, &dash_font_12, 0xE7FFE9, jstr(msg, "text", ""));
    lv_obj_set_width(l2, LV_PCT(100));
    lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
    s_ntf_t0 = esp_timer_get_time();
    s_ntf_close = false;
    s_ntf_timer = lv_timer_create(ntf_timer_cb, 40, NULL);
    ESP_LOGI(TAG, "aviso de mensagem (%s)", s_ntf_mem ? "com animação" : "só a faixa");
}

static void housekeeping_cb(lv_timer_t *t)
{
    static uint32_t ticks;
    ticks++;
    bool alive = link_host_alive();
    lv_obj_set_style_text_color(s_link, lv_color_hex(alive ? C_OK : C_ERR), 0);
    if (!alive) {
        if (s_was_alive) {
            lv_label_set_text(s_status, "offline");
        }
        if (ticks % 6 == 0) {   // ~3 s: anuncia-se até o host responder
            send_hello();
        }
    }
    s_was_alive = alive;
    if (!s_asleep && s_sleep_ms && s_has_screen && !s_clock && !strcmp(s_screen_id, "home") &&
        lv_display_get_inactive_time(NULL) > s_sleep_ms) {
        sleep_enter();
    }
}

static void button_poll_cb(lv_timer_t *t)
{
    static bool prev;
    bool now = board_boot_pressed();
    if (now && !prev) {
        if (s_asleep) wake_up();
        else go_back();
    }
    prev = now;
}

// ---------- API ----------

void ui_init(lv_display_t *disp)
{
    lvgl_port_lock(0);

    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(C_ACCENT), lv_color_hex(C_OK), true,
                                           &dash_font_14);
    lv_display_set_theme(disp, th);
    emoji_fonts_init();
    // As camadas de cima (confirmações Não/Sim, toasts) não herdam a fonte do ecrã: sem isto usam a
    // Montserrat do LVGL, que não tem acentos (o "ã" de "Não" aparecia como um quadrado).
    lv_obj_set_style_text_font(lv_layer_top(), &dash_font_14, 0);
    lv_obj_set_style_text_font(lv_layer_sys(), &dash_font_14, 0);

    s_W = lv_display_get_horizontal_resolution(disp);
    s_H = lv_display_get_vertical_resolution(disp);
    s_land = s_W > s_H;
    s_rot = (uint8_t)lv_display_get_rotation(disp);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Barra superior
    lv_obj_t *bar = plain_box(scr);
    lv_obj_set_size(bar, s_W, BAR_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_back_btn = lv_obj_create(bar);
    lv_obj_remove_style_all(s_back_btn);
    lv_obj_set_size(s_back_btn, 26, BAR_H);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_back_btn, 8);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
    lv_obj_t *bl = mk_label(s_back_btn, &dash_font_16, C_ACCENT, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);

    s_title = mk_label(bar, &dash_font_14, C_TEXT, "DashDeck");
    lv_obj_set_flex_grow(s_title, 1);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    s_pager = plain_box(bar);
    lv_obj_set_size(s_pager, LV_SIZE_CONTENT, 6);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pager, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_pager, 3, 0);
    lv_obj_set_style_pad_right(s_pager, 4, 0);
    lv_obj_add_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
    s_status = mk_label(bar, &dash_font_12, C_SUB, "");
    s_link = mk_label(bar, &dash_font_12, C_ERR, LV_SYMBOL_USB);

    // Conteúdo com rolagem
    s_content = lv_obj_create(scr);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 0, BAR_H);
    lv_obj_set_size(s_content, s_W, s_H - BAR_H);
    lv_obj_set_style_pad_all(s_content, 6, 0);
    lv_obj_set_style_pad_row(s_content, 8, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    s_body = plain_box(s_content);
    lv_obj_set_size(s_body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 6, 0);
    lv_obj_set_style_pad_column(s_body, 6, 0);

    s_btns = plain_box(s_content);
    lv_obj_set_size(s_btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_btns, 6, 0);
    lv_obj_set_style_pad_column(s_btns, 6, 0);

    // Dock (comandos fixos em baixo)
    s_dock = plain_box(scr);
    lv_obj_set_pos(s_dock, 0, s_H - DOCK_H);
    lv_obj_set_size(s_dock, s_W, DOCK_H);
    lv_obj_set_style_bg_opa(s_dock, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dock, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_border_side(s_dock, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_dock, 1, 0);
    lv_obj_set_style_border_color(s_dock, lv_color_hex(C_CARD_PR), 0);
    lv_obj_set_style_pad_all(s_dock, 2, 0);
    lv_obj_set_flex_flow(s_dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);

    // Toast
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, 160, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -DOCK_H - 6);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_set_style_pad_all(s_toast, 8, 0);
    lv_obj_t *tl = mk_label(s_toast, &dash_font_14, C_TEXT, "");
    lv_obj_set_width(tl, LV_PCT(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_list = s_body;
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    // a animação é a tela de carregamento; o spinner "Aguardando uConsole" só fica se faltar memória
    show_waiting();
    if (boot_start()) lv_obj_clean(s_body);
    lv_timer_create(housekeeping_cb, 500, NULL);
    lv_timer_create(button_poll_cb, 40, NULL);

    lvgl_port_unlock();
    send_hello();
}

void ui_handle(const cJSON *msg)
{
    const char *t = jstr(msg, "t", "");

    // Mensagens que não mexem no LVGL
    if (!strcmp(t, "ping")) {
        link_send_event("pong", NULL, NULL, NULL, NULL);
        return;
    }
    if (!strcmp(t, "hello?")) {
        send_hello();
        return;
    }
    if (!strcmp(t, "rot")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, "v");
        if (cJSON_IsNumber(v) && (v->valueint & 3) != s_rot) {
            nvs_handle_t h;
            if (nvs_open("dash", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "rot", (uint8_t)(v->valueint & 3));
                nvs_commit(h);
                nvs_close(h);
                ESP_LOGI(TAG, "rotação %d: reiniciando", v->valueint & 3);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        return;
    }
    if (!strcmp(t, "bg")) {
        handle_bg(msg);
        return;
    }
    if (!strcmp(t, "bl")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, "v");
        if (cJSON_IsNumber(v)) {
            uint8_t bl = (uint8_t)(v->valueint < 0 ? 0 : v->valueint > 100 ? 100 : v->valueint);
            if (s_clock) {
                s_bl_saved = bl;   // o relógio tem o seu brilho; este volta ao sair
            } else if (s_asleep) {
                s_bl = bl;         // aplicado ao acordar
            } else {
                s_bl = bl;
                board_set_backlight(bl);
            }
        }
        return;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "lock LVGL timeout (%s)", t);
        return;
    }
    if (!strcmp(t, "screen")) {
        render_screen(msg);
    } else if (!strcmp(t, "update")) {
        if (s_has_screen && !strcmp(jstr(msg, "id", ""), s_screen_id)) {
            set_pager(msg);
            apply_body(msg, false);
            const char *title = jstr(msg, "title", NULL);
            if (title) lv_label_set_text(s_title, title);
        }
    } else if (!strcmp(t, "notify")) {
        ntf_show(msg);
    } else if (!strcmp(t, "idle")) {
        int v = jint(msg, "home", 0);
        s_sleep_ms = v > 0 ? (uint32_t)v * 1000 : 0;
        if (!s_sleep_ms) wake_up();
    } else if (!strcmp(t, "time")) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(msg, "epoch");
        if (cJSON_IsNumber(e)) {
            s_epoch_off = (int64_t)(e->valuedouble * 1e6) - esp_timer_get_time();
            s_has_time = true;
        }
    } else if (!strcmp(t, "status")) {
        lv_label_set_text(s_status, jstr(msg, "text", ""));
    } else if (!strcmp(t, "dock")) {
        build_dock(cJSON_GetObjectItemCaseSensitive(msg, "items"));
        layout_content(s_has_screen ? s_dock_want : true);
    } else if (!strcmp(t, "icon")) {
        handle_icon(msg);
    } else if (!strcmp(t, "emoji")) {
        handle_emoji(msg);
    } else if (!strcmp(t, "toast")) {
        show_toast(jstr(msg, "text", ""), jstr(msg, "level", "info"));
    } else {
        ESP_LOGW(TAG, "tipo desconhecido: %s", t);
    }
    lvgl_port_unlock();
}
