// DashDeck UI: renderizador genérico controlado pelo host (dashd no uConsole).
//
// Host -> ESP
//   (descrição completa: docs/arquitetura.md)
//   {"t":"screen","id":"home","tpl":"menu|list|kv|log|text|plot","title":"..","back":false,
//    "items":[{"id","label","sub","icon","color","right"}], "rows":[["k","v","#color"?]],
//    "lines":[".."], "text":"..", "buttons":[{"id","label","color","confirm"}]}
//   {"t":"update","id":"..", <mesmos campos de dados>, "append":[".."]}
//   {"t":"hdr","plane":0|1|2}  avião no cabeçalho: monitor de emergências ligado (1 laranja, 2 vermelho)
//   {"t":"status","text":"12:34"}   {"t":"toast","text":"..","level":"ok|warn|err|info"}
//   {"t":"bl","v":0..100}           {"t":"ping"}   {"t":"hello?"}
//   Páginas: "pages":N,"page":i no screen/update mostram pontos; o swipe lateral envia "swipe".
//   tpl "plot": canvas w x h (px) + linhas kv ("rows") ao lado (se couber) ou por baixo:
//    "plot":{"w":308,"h":136,"cx":..,"cy":..,"R":..,"rings":[px,..],"rlabels":["","50nm"],
//            "cardinal":true,"cross":true,"center":true,"msg":"..","bg":"chave",
//            "pts":[[x,y,"#cor","rótulo",rumo|null,cheio(0/1),"plane|ship|balloon"?],..],   px do canvas
//            "tag":[x,y,"#cor","linha1\nlinha2.."]   etiqueta do alvo seguido (até 4 linhas)
//            "drag":true  mapa arrastável: ao soltar -> btn "pan:dx,dy"; dois toques -> btn "mapinfo";
//                         um toque (sem 2.º em 0,4 s) -> btn "tap:x,y" (px do canvas);
//                         arrasto a começar na borda esquerda/direita = swipe de página
//            "tracks":[["#cor",largura(1|2),"xxyyxxyy.."],..],
//            "texts":[[x,y,"#cor","texto","l|c|r"],..]  textos em fonte 12 (y = meio), ex.: eixos
//            "ctrl":[["id","tl|tr|bl|br|l|r|t|b","rótulo"],..]}  botões por cima do canvas (ex.: setas
//            e zoom do mapa) -> {"t":"btn","id":screen,"btn":id}; rótulo vazio nas bordas = seta  trajetos por baixo dos pontos; cada
//            coordenada são 2 caracteres base64url (A-Z a-z 0-9 - _) com valor px+1024
//   {"t":"bg","key":"..","w":..,"h":..,"pal":["#rrggbb"x16],"off":n,"d":"base64","last":true}
//      imagem de fundo 4 bpp (2 px/byte, nibble alto primeiro) usada pelo plot com o mesmo "bg";
//      cada pedaço é confirmado com {"t":"bgack","off":n,"ok":bool}
//   {"t":"jpg","id":screen,"off":n,"size":total,"d":"base64","last":true}  JPEG (até 24 KB) desenhado no
//      canvas do plot desta tela (centrado; reduzido 1/2..1/8 se não couber) -> {"t":"jpgack","ok","ms"}
//      plot "live":true: num update o canvas não é redesenhado (fica a última imagem); "ctrl" aceita
//      uma 4.ª posição com a cor do botão, e a largura acompanha o rótulo
//   {"t":"rot","v":0..3}  grava a rotação na NVS e reinicia
//   {"t":"icon","id":"..","w":32,"h":32,"d":"base64 RGB565A8"} -> {"t":"iconack","id":..,"ok":..}
//   {"t":"dock","items":[{"id","label","icon","img","color"}]}  barra fixa em baixo -> {"t":"dock","item":id}
//   screen: "layout":"grid" (menu em boxes) e "dock":false (esconde o dock nesta tela)
//   {"t":"icon","id":..,"probe":true} (sem dados): ok=true se o ícone já está no cartão SD
//   {"t":"emoji","cp":0x100000+i,"w":15,"h":14,"d":"base64 RGB565A8"} -> {"t":"emojiack","cp":..,"ok":..}
//      imagem do emoji que o host pôs no texto como o código privado cp (até 48)
//   item de menu: "badge":"3" mostra um selo vermelho (ex.: mensagens não lidas); nos boxes,
//      "bg" (fundo) e "sub_color" (cor do subtítulo), ex.: interruptores do AIO v2; "value" (e
//      "value_color"): valor em destaque, em letra maior, entre o nome (1 linha) e o subtítulo;
//      "dot":"#cor" ponto no canto superior direito; "mark":"up|down" + "mark_color" seta ↑/↓ no
//      canto superior esquerdo (só criados se o campo vier ao construir a tela)
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
//   "pet":[x da casa, base, luz 0..100, 1 brinca|0 dorme]: o cão junto à casa (só na paisagem).
//   Swipe lateral troca o mostrador -> {"t":"clock","face":"1"}; vertical muda o brilho ->
//   {"t":"clock","bl":"60"}; um toque pede confirmação e o 2.º (em 2,5 s) volta -> {"t":"back"}.
//   "dim":s,"dim_bl":% no clock: escurece após s segundos sem toques; o toque seguinte repõe o brilho.
//   Mostradores (face): 0 paisagem, 1 retro, 2 previsão, 3 cidade, 4 temporizador e alarme.
//   "timer":{"set":s,"end":epoch local|0,"left":s em pausa|0}, "alarm":{"h","m","on"} (mostrador 4).
//   No mostrador 4 os toques nas zonas enviam {"t":"clock","tset":"300"}, {"tact":"start|pause|reset"},
//   {"alarm":"07:30"}, {"aon":"1"}; manter o dedo numa seta repete. No retro, um toque longo muda a cor
//   do LCD -> {"t":"clock","pal":"1"}.
//   notify: "kind":"alarm" (despertador dourado) ou "squawk" (avião em emergência, vermelho),
//   "title" (1.ª linha da faixa), "dur" (s até fechar).
//   Os gestos do relógio são medidos do toque ao soltar (não pelo detetor do LVGL). O brilho normal volta ao sair.
#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "bootanim.h"
#include "notify.h"
#include "aboutanim.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "clockface.h"
#include "dash_fonts.h"
#include "esp_timer.h"
#include "esp32c6/rom/tjpgd.h"
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
static int64_t s_clk_dsum, s_clk_dmax;   // tempo do desenho do relógio (stats)
static int s_clk_dn;
static lv_obj_t *s_plot_wrap;      // canvas + botões de controlo por cima
#define CTRL_MAX 10
static char s_ctrl_ids[CTRL_MAX][ID_LEN];
static char s_ctrl_sig[256];
static lv_draw_buf_t s_cbuf;
static uint8_t *s_cmem;
// Buffer de ecrã inteiro reservado no arranque e reutilizado pelo canvas (relógio, mapas) e pela
// animação de arranque: pedir e devolver 110 KB a cada tela fragmentava a memória até não haver um bloco
// contínuo para o relógio.
static uint8_t *s_arena;
static size_t s_arena_size;
static void boot_end(void);
static void ts_hook(void);
static lv_obj_t *s_boot;
static int32_t s_cw, s_ch;
// Imagem de fundo (4 bpp + paleta de 16, ou 8 bpp + paleta de 256 no relógio)
static uint8_t *s_bg;
static size_t s_bg_size;
static int32_t s_bg_w, s_bg_h;
static uint8_t s_bg_bpp = 4;
static bool s_bg_on_sd;            // o fundo está em BG_FILE (sem RAM para ele)
static FILE *s_bg_fw;              // a ser escrito
static bool s_ntf_borrowed;        // o canvas do aviso está a usar a memória do fundo (relógio)
static int s_ntf_arena;            // o aviso desenha na arena: 1 = estava livre, 2 = tirada ao plot
static void ntf_drop_canvas(void);
static uint16_t s_bg_pal[256];
static char s_bg_key[32];
static bool s_bg_ready;
static size_t s_bg_next;     // próximo offset esperado
// Relógio
static lv_obj_t *s_clock;          // ecrã inteiro por cima de tudo
static lv_timer_t *s_clock_timer;
static clock_state_t s_cs = {.pet = -1};
static uint32_t s_clock_full_ms;   // último desenho do ecrã inteiro
// O fundo da faixa onde o cão corre, guardado na RAM a cada desenho completo: os quadros do cão só
// repõem esta faixa e desenham o cão (o fundo inteiro pode estar no cartão, lento demais para ~14 qps).
static uint16_t *s_pet_bg;
static int s_pet_r[4];
static bool s_pet_ok;
static int s_pet_frames;   // stats
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
static int s_press_zone;          // zona do temporizador tocada (CZ_*)
static bool s_long_done;          // o toque longo já fez algo (cor do LCD, repetição das setas)
static bool s_edit_tm, s_edit_al; // há ajustes por enviar ao host
static uint32_t s_edit_until;     // até lá, ignora os valores do host (pode vir um update antigo)
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
static lv_obj_t *s_lbl_d[MAX_ITEMS];             // valor em destaque da box da grade ("value") ou NULL
static lv_obj_t *s_dot[MAX_ITEMS];               // ponto colorido no canto superior direito ("dot") ou NULL
static lv_obj_t *s_mark[MAX_ITEMS];              // seta no canto superior esquerdo ("mark") ou NULL
static lv_obj_t *s_log_lbl;
static bool s_is_img[MAX_ITEMS];                 // s_lbl_c[i] é uma imagem (ícone de app)
static lv_obj_t *s_badge[MAX_ITEMS];             // selo vermelho no canto (menu)
static char s_btn_ids[MAX_BTNS][ID_LEN];

// Ícones de aplicações (RGB565A8) enviados pelo host
#define ICON_MAX 96
#define ICON_PX  48
// Ícones ficam também na RAM (desenho rápido, sem ler o cartão no barramento do display) enquanto
// sobrar pelo menos isto de heap. A RAM vai para os ícones da tela aberta e do dock: ao construir uma
// tela, os que lá aparecem carregam-se do cartão e libertam as cópias de telas que já não se veem.
// Os que não couberem desenham-se do cartão através da cache de imagens.
// (36 KB até à 0.25.0: depois de algum uso, os ícones das Aplicações ficavam no cartão e o scroll
// bloqueava ~400 ms; com todos na RAM sobram ~24 KB e tudo funciona.)
#define ICON_RAM_RESERVE (20 * 1024)
typedef struct {
    char id[24];
    lv_image_dsc_t dsc;
    uint8_t *data;      // cópia na RAM (se houver memória), usada em vez do cartão
    char path[24];      // no cartão: "S:dash/xxxxxxxx.bin" (persiste entre arranques)
    uint32_t gen;       // última tela (s_icon_gen) que o usou
    uint32_t tried;     // tela em que já se tentou trazê-lo para a RAM (não repete a cada update)
} icon_t;
#define ICON_USED(i) (s_icons[i].data || s_icons[i].path[0])
static bool s_sd_ok;    // cartão SD montado (ícones e fundos grandes vão para lá)
#define SD_DIR BOARD_SD_PATH "/dash"
#define BG_FILE SD_DIR "/bg.bin"
static icon_t s_icons[ICON_MAX];
static int s_icon_next;
static uint32_t s_icon_gen = 1;   // sobe a cada tela nova
static void make_badge(lv_obj_t *card, int i, const cJSON *it);
static void ask_confirm(const char *text, uint32_t yes_color);
static void draw_tracks(const cJSON *tracks);
static void draw_icons(const cJSON *pts);
static void about_end(void);
static void draw_tag(lv_layer_t *layer, const cJSON *tag, int32_t w, int32_t h);
static void build_ctrl(const cJSON *ctrl);

static lv_obj_t *s_dock;
static char s_dock_ids[DOCK_MAX][ID_LEN];
static char s_dock_imgs[DOCK_MAX][24];   // ícones do dock: nunca são substituídos por outros
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
        {"sd", LV_SYMBOL_SD_CARD},    {"folder", LV_SYMBOL_DIRECTORY},
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

static void icon_load_ram(icon_t *slot);

// Fonte da imagem do ícone para lv_image_set_src: o descritor na RAM ou o ficheiro no cartão.
// Marca-o como usado pela tela atual e, se só estiver no cartão, tenta trazê-lo para a RAM.
static const void *icon_find(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < ICON_MAX; i++) {
        if (ICON_USED(i) && !strcmp(s_icons[i].id, id)) {
            icon_t *slot = &s_icons[i];
            slot->gen = s_icon_gen;
            if (!slot->data && slot->path[0] && slot->tried != s_icon_gen) {
                slot->tried = s_icon_gen;
                icon_load_ram(slot);
            }
            return slot->data ? (const void *)&slot->dsc : (const void *)slot->path;
        }
    }
    return NULL;
}

static void icon_set_ram(icon_t *slot, uint8_t *buf, int w, int h, size_t size)
{
    memset(&slot->dsc, 0, sizeof(slot->dsc));
    slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    slot->dsc.header.w = w;
    slot->dsc.header.h = h;
    slot->dsc.header.stride = w * 2;
    slot->dsc.data_size = size;
    slot->dsc.data = buf;
    slot->data = buf;
}

static bool icon_ram_ok(size_t size)
{
    return esp_get_free_heap_size() > ICON_RAM_RESERVE + size;
}

// Lugar para o ícone `id`: o que já o tem, um livre, ou (cheio) o seguinte em rotação que não seja
// do dock. O ícone substituído é comunicado ao host ({"t":"iconlost"}) para ele o voltar a enviar.
static icon_t *icon_slot(const char *id)
{
    for (int i = 0; i < ICON_MAX; i++)
        if (ICON_USED(i) && !strcmp(s_icons[i].id, id)) return &s_icons[i];
    for (int i = 0; i < ICON_MAX; i++)
        if (!ICON_USED(i)) return &s_icons[i];
    icon_t *slot = NULL;
    for (int k = 0; k < ICON_MAX && !slot; k++) {
        icon_t *c = &s_icons[s_icon_next];
        s_icon_next = (s_icon_next + 1) % ICON_MAX;
        bool dock = false;
        for (int d = 0; d < DOCK_MAX && !dock; d++) dock = s_dock_imgs[d][0] && !strcmp(s_dock_imgs[d], c->id);
        if (!dock) slot = c;
    }
    if (!slot) slot = &s_icons[0];
    link_send_event("iconlost", "id", slot->id, NULL, NULL);
    return slot;
}

static void icon_clear(icon_t *slot)
{
    if (slot->data) {
        lv_image_cache_drop(&slot->dsc);
        free(slot->data);
        slot->data = NULL;
    }
    if (slot->path[0]) lv_image_cache_drop(slot->path);
    slot->path[0] = '\0';
}

static bool icon_is_dock(const icon_t *slot)
{
    for (int d = 0; d < DOCK_MAX; d++)
        if (s_dock_imgs[d][0] && !strcmp(s_dock_imgs[d], slot->id)) return true;
    return false;
}

// Traz para a RAM um ícone que só está no cartão. Para caber, esvazia a cache de imagens e liberta as
// cópias na RAM de ícones que a tela atual e o dock não usam (os widgets deles já foram apagados).
static void icon_load_ram(icon_t *slot)
{
    char file[48];
    snprintf(file, sizeof(file), BOARD_SD_PATH "/%s", slot->path + 2);   // "S:dash/x.bin" -> /sd/dash/x.bin
    const size_t size = 32 * 32 * 3;   // ícones de 32 px (RGB565A8)
    if (!icon_ram_ok(size)) lv_image_cache_drop(NULL);
    for (int i = 0; i < ICON_MAX && !icon_ram_ok(size); i++) {
        icon_t *o = &s_icons[i];
        if (o->data && o->gen != s_icon_gen && !icon_is_dock(o)) {
            lv_image_cache_drop(&o->dsc);
            free(o->data);
            o->data = NULL;
        }
    }
    if (!icon_ram_ok(size)) return;
    FILE *f = fopen(file, "rb");
    if (!f) return;
    lv_image_header_t hdr;
    uint8_t *buf = NULL;
    if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == LV_IMAGE_HEADER_MAGIC &&
        (size_t)hdr.w * hdr.h * 3 <= (size_t)ICON_PX * ICON_PX * 3) {
        size_t n = (size_t)hdr.w * hdr.h * 3;
        buf = malloc(n);
        if (buf && fread(buf, 1, n, f) == n) {
            lv_image_cache_drop(slot->path);
            icon_set_ram(slot, buf, hdr.w, hdr.h, n);
            buf = NULL;
        }
    }
    free(buf);
    fclose(f);
}

// Grava o ícone como imagem do LVGL (cabeçalho + RGB565A8) no cartão. Devolve false se falhar.
static bool icon_to_sd(icon_t *slot, const char *id, int w, int h, const uint8_t *buf, size_t size)
{
    size_t n = strlen(id);
    const char *tail = n > 8 ? id + n - 8 : id;   // nomes 8.3 (o FAT do ESP não tem nomes longos)
    char file[48];
    snprintf(file, sizeof(file), SD_DIR "/%s.bin", tail);
    FILE *f = fopen(file, "wb");
    if (!f) return false;
    lv_image_header_t hdr = {0};
    hdr.magic = LV_IMAGE_HEADER_MAGIC;
    hdr.cf = LV_COLOR_FORMAT_RGB565A8;
    hdr.w = w;
    hdr.h = h;
    hdr.stride = w * 2;
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 && fwrite(buf, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (ok) snprintf(slot->path, sizeof(slot->path), "S:dash/%s.bin", tail);
    return ok;
}

// O ícone já está no cartão (de um arranque anterior)? Se estiver, fica registado sem o host o reenviar.
// O ícone já está no cartão (de um arranque anterior)? Se estiver, fica registado sem o host o reenviar.
// A leitura do cartão faz-se sem o lock do LVGL (o ecrã continua a desenhar); só o registo o usa.
// Corre na tarefa da ligação, a única que muda os lugares dos ícones fora do LVGL.
static bool icon_probe(const char *id)
{
    if (!s_sd_ok || !id[0]) return false;
    size_t n = strlen(id);
    const char *tail = n > 8 ? id + n - 8 : id;
    char file[48];
    snprintf(file, sizeof(file), SD_DIR "/%s.bin", tail);
    struct stat st;
    if (stat(file, &st) != 0 || st.st_size <= (off_t)sizeof(lv_image_header_t)) return false;
    bool in_ram = false;
    for (int i = 0; i < ICON_MAX && !in_ram; i++)
        in_ram = s_icons[i].data && !strcmp(s_icons[i].id, id);
    if (in_ram) return true;
    // cópia na RAM, se houver memória: desenhar do cartão atrasa o display (mesmo barramento SPI)
    size_t size = (size_t)st.st_size - sizeof(lv_image_header_t);
    uint8_t *buf = NULL;
    lv_image_header_t hdr = {0};
    if (icon_ram_ok(size)) {
        FILE *f = fopen(file, "rb");
        buf = f ? malloc(size) : NULL;
        if (!(f && buf && fread(&hdr, sizeof(hdr), 1, f) == 1 && fread(buf, 1, size, f) == size &&
              hdr.magic == LV_IMAGE_HEADER_MAGIC && (size_t)hdr.w * hdr.h * 3 == size)) {
            free(buf);
            buf = NULL;
        }
        if (f) fclose(f);
    }
    if (!lvgl_port_lock(1000)) {
        free(buf);
        return false;
    }
    icon_t *slot = icon_slot(id);
    icon_clear(slot);
    strlcpy(slot->id, id, sizeof(slot->id));
    snprintf(slot->path, sizeof(slot->path), "S:dash/%s.bin", tail);
    if (buf) icon_set_ram(slot, buf, hdr.w, hdr.h, size);
    lvgl_port_unlock();
    return true;
}

static void handle_icon(const cJSON *msg)
{
    const char *id = jstr(msg, "id", "");
    // (o "probe" trata-se em ui_handle, fora do lock do LVGL)
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(msg, "w"), *jh = cJSON_GetObjectItemCaseSensitive(msg, "h");
    int w = cJSON_IsNumber(jw) ? jw->valueint : 0, h = cJSON_IsNumber(jh) ? jh->valueint : 0;
    bool ok = false;
    if (id[0] && w > 0 && h > 0 && w <= ICON_PX && h <= ICON_PX) {
        size_t size = (size_t)w * h * 3, olen = 0;
        uint8_t *buf = malloc(size);
        const char *d = jstr(msg, "d", "");
        if (buf && mbedtls_base64_decode(buf, size, &olen, (const unsigned char *)d, strlen(d)) == 0 && olen == size) {
            icon_t *slot = icon_slot(id);
            icon_clear(slot);
            strlcpy(slot->id, id, sizeof(slot->id));
            bool on_sd = s_sd_ok && icon_to_sd(slot, id, w, h, buf, size);
            if (on_sd && !icon_ram_ok(size)) {   // sem memória: fica só no cartão
                free(buf);
                ok = true;
                goto ack;
            }
            icon_set_ram(slot, buf, w, h, size);
            ok = true;
        } else {
            free(buf);
        }
    }
ack:;
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

    const void *img = icon_find(jstr(it, "img", NULL));
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

// Mudanças só quando o valor é outro: refazer texto, cor ou imagem iguais invalida e refaz o layout,
// e uma imagem do cartão voltava a ser aberta e lida no SD. Com atualizações a cada 2 s isto
// bloqueava o LVGL até 270 ms a meio dos swipes.
static void set_text(lv_obj_t *o, const char *t)
{
    const char *cur = lv_label_get_text(o);
    if (!cur || strcmp(cur, t)) lv_label_set_text(o, t);
}

static void set_text_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}

static void set_bg_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), c)) lv_obj_set_style_bg_color(o, c, 0);
}

static void set_hidden(lv_obj_t *o, bool hide)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hide) {
        if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_img(lv_obj_t *o, const void *src)
{
    const void *cur = lv_image_get_src(o);
    if (cur == src) return;
    if (cur && lv_image_src_get_type(src) == LV_IMAGE_SRC_FILE && lv_image_src_get_type(cur) == LV_IMAGE_SRC_FILE &&
        !strcmp(cur, src)) return;
    lv_image_set_src(o, src);
}

static void set_badge(int i, const cJSON *it)
{
    const char *b = jstr(it, "badge", "");
    if (!s_badge[i]) return;
    set_text(lv_obj_get_child(s_badge[i], 0), b);
    set_hidden(s_badge[i], !b[0]);
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
    const void *img = icon_find(jstr(it, "img", NULL));
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
    set_bg_color(card, parse_color(jstr(it, "bg", NULL), C_CARD));
    set_text_color(sub, parse_color(jstr(it, "sub_color", NULL), C_SUB));
}

// Cantos da box: "dot":"#cor" (ponto no canto superior direito, ex.: bolsa aberta/fechada) e
// "mark":"up|down|" com "mark_color" (seta no canto superior esquerdo, ex.: ação a subir/descer).
static const char *mark_text(const cJSON *it)
{
    const char *m = jstr(it, "mark", "");
    return !strcmp(m, "up") ? "\xE2\x86\x91" : !strcmp(m, "down") ? "\xE2\x86\x93" : "";   // ↑ ↓
}

static void set_corners(int i, const cJSON *it)
{
    if (s_dot[i]) {
        const char *d = jstr(it, "dot", "");
        set_hidden(s_dot[i], !d[0]);
        if (d[0]) set_bg_color(s_dot[i], parse_color(d, C_SUB));
    }
    if (s_mark[i]) {
        const char *m = mark_text(it);
        set_text(s_mark[i], m);
        set_hidden(s_mark[i], !m[0]);
        set_text_color(s_mark[i], parse_color(jstr(it, "mark_color", NULL), C_TEXT));
    }
}

static void make_corners(lv_obj_t *card, int i, const cJSON *it)
{
    s_dot[i] = s_mark[i] = NULL;
    if (cJSON_GetObjectItemCaseSensitive(it, "dot")) {
        lv_obj_t *d = lv_obj_create(card);
        lv_obj_remove_style_all(d);
        lv_obj_add_flag(d, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_TOP_RIGHT, -3, 0);
        s_dot[i] = d;
    }
    if (cJSON_GetObjectItemCaseSensitive(it, "mark")) {
        lv_obj_t *m = mk_label(card, &dash_arrows, C_TEXT, "");   // seta grossa de 22 px
        lv_obj_add_flag(m, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(m, LV_ALIGN_TOP_LEFT, 2, -6);
        s_mark[i] = m;
    }
    set_corners(i, it);
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
    lv_obj_set_style_pad_top(card, s_hgrid ? 5 : 6, 0);
    lv_obj_set_style_pad_bottom(card, 5, 0);
    lv_obj_set_style_pad_hor(card, 3, 0);
    lv_obj_set_style_pad_row(card, s_hgrid ? 4 : 2, 0);
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
    const char *val = jstr(it, "value", NULL);
    lv_obj_t *t = mk_label(card, nf, C_TEXT, jstr(it, "label", "?"));
    if (s_hgrid && !val) {
        // fileira da tela inicial: o nome só ocupa as linhas que tem (até 2), para o ícone e os
        // textos ficarem centrados na box (com 2 linhas fixas, um nome curto deixava um vazio por baixo)
        lv_obj_set_size(t, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(t, 2 * lv_font_get_line_height(nf), 0);
    } else {
        lv_obj_set_size(t, LV_PCT(100), (val ? 1 : 2) * lv_font_get_line_height(nf));
    }
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    s_lbl_d[i] = NULL;
    if (val) {   // valor em destaque (ex.: cotação), em letra maior por baixo do nome
        const lv_font_t *vf = s_hgrid ? &dash_font_20 : &dash_font_16;
        lv_obj_t *v = mk_label(card, vf, C_TEXT, val);
        lv_obj_set_size(v, LV_PCT(100), lv_font_get_line_height(vf));
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        set_text_color(v, parse_color(jstr(it, "value_color", NULL), C_TEXT));
        s_lbl_d[i] = v;
    }
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
    make_corners(card, i, it);
    make_badge(card, i, it);
}

static void update_menu_item(int i, const cJSON *it)
{
    set_badge(i, it);
    if (s_grid) tile_style(lv_obj_get_parent(s_lbl_a[i]), s_lbl_b[i], it);
    set_text(s_lbl_a[i], jstr(it, "label", "?"));
    if (s_grid) set_corners(i, it);
    if (s_grid && s_lbl_d[i]) {
        set_text(s_lbl_d[i], jstr(it, "value", ""));
        set_text_color(s_lbl_d[i], parse_color(jstr(it, "value_color", NULL), C_TEXT));
    }
    const char *sub = jstr(it, "sub", "");
    set_text(s_lbl_b[i], sub);
    set_hidden(s_lbl_b[i], !sub[0]);
    const void *img = icon_find(jstr(it, "img", NULL));
    if (s_is_img[i]) {
        if (img) set_img(s_lbl_c[i], img);
    } else {
        set_text(s_lbl_c[i], icon_symbol(jstr(it, "icon", NULL)));
        set_text_color(s_lbl_c[i], parse_color(jstr(it, "color", NULL), C_ACCENT));
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
    set_text(s_lbl_a[i], jstr(it, "label", ""));
    set_text_color(s_lbl_a[i], parse_color(jstr(it, "color", NULL), C_TEXT));
    set_text(s_lbl_b[i], jstr(it, "sub", ""));
    set_text(s_lbl_c[i], jstr(it, "right", ""));
    lv_obj_t *row = lv_obj_get_parent(s_lbl_c[i]);
    set_bg_color(row, parse_color(jstr(it, "bg", NULL), C_CARD));
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
    set_text(s_lbl_a[i], cJSON_IsString(k) ? k->valuestring : "");
    set_text(s_lbl_b[i], cJSON_IsString(v) ? v->valuestring : "");
    set_text_color(s_lbl_b[i], parse_color(cJSON_IsString(c) ? c->valuestring : NULL, C_TEXT));
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

static uint32_t s_btn_sig;   // assinatura dos botões atuais (num update iguais não se refazem)

static uint32_t fnv(uint32_t h, const char *t)
{
    for (; *t; t++) h = (h ^ (uint8_t)*t) * 16777619u;
    return (h ^ 0xFF) * 16777619u;
}

static void build_buttons(const cJSON *btns)
{
    uint32_t sig = 2166136261u;
    const cJSON *bb;
    cJSON_ArrayForEach(bb, btns) {
        sig = fnv(fnv(fnv(fnv(sig, jstr(bb, "id", "")), jstr(bb, "label", "")), jstr(bb, "color", "")),
                  jstr(bb, "confirm", ""));
    }
    if (sig == s_btn_sig && lv_obj_get_child_count(s_btns) == (uint32_t)cJSON_GetArraySize(btns)) return;
    s_btn_sig = sig;
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
    if (s_arena) {
        uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
        if (stride * h > s_arena_size) return false;
        if (s_ntf_arena) ntf_drop_canvas();   // o aviso estava na arena: fica só a faixa
        if (s_boot) boot_end();   // a animação de arranque usa o mesmo buffer
        if (w != s_cw || h != s_ch || s_cmem != s_arena || lv_canvas_get_draw_buf(s_canvas) != &s_cbuf) {
            lv_draw_buf_init(&s_cbuf, w, h, LV_COLOR_FORMAT_RGB565, stride, s_arena, stride * h);
            lv_canvas_set_draw_buf(s_canvas, &s_cbuf);
            s_cmem = s_arena;
            s_cw = w;
            s_ch = h;
        }
        return true;
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

// Fundo guardado no cartão: lê-o em blocos de linhas direto para o canvas.
static void paint_bg_sd(void)
{
    static uint8_t blk[8192];   // blocos grandes: o FAT lê vários setores de uma vez
    static int s_n;
    static int64_t s_us;
    int64_t t0 = esp_timer_get_time();
    size_t rowb = s_bg_bpp == 8 ? (size_t)s_cw : (size_t)(s_cw + 1) / 2;
    int rows = (int)(sizeof(blk) / rowb);
    FILE *f = fopen(BG_FILE, "rb");
    if (!f) {
        lv_canvas_fill_bg(s_canvas, lv_color_hex(C_BG), LV_OPA_COVER);
        return;
    }
    if (s_bg_bpp == 4 && (s_cw & 1)) rows = 1;   // 4 bpp com largura ímpar: lido linha a linha à mão
    for (int32_t y0 = 0; y0 < s_ch; y0 += rows) {
        int n = (int)(s_ch - y0 < rows ? s_ch - y0 : rows);
        size_t want = s_bg_bpp == 8 ? rowb * n : (size_t)((s_cw * n + 1) / 2);
        if (fread(blk, 1, want, f) != want) break;
        for (int k = 0; k < n; k++) {
            uint16_t *row = (uint16_t *)(s_cbuf.data + (y0 + k) * s_cbuf.header.stride);
            for (int32_t x = 0; x < s_cw; x++) {
                if (s_bg_bpp == 8) {
                    row[x] = s_bg_pal[blk[k * rowb + x]];
                } else {
                    size_t i = (size_t)k * s_cw + x;
                    row[x] = s_bg_pal[(i & 1) ? (blk[i / 2] & 0x0F) : (blk[i / 2] >> 4)];
                }
            }
        }
    }
    fclose(f);
    s_us += esp_timer_get_time() - t0;
    if (++s_n == 40) {   // de vez em quando, quanto custa ler o fundo do cartão
        ESP_LOGI(TAG, "fundo do cartão: %d ms por quadro", (int)(s_us / s_n / 1000));
        s_n = 0;
        s_us = 0;
    }
}

static void paint_bg(const char *key)
{
    if (!key || !s_bg_ready || strcmp(key, s_bg_key) || s_bg_w != s_cw || s_bg_h != s_ch) {
        lv_canvas_fill_bg(s_canvas, lv_color_hex(C_BG), LV_OPA_COVER);
        return;
    }
    if (s_bg_on_sd) {
        paint_bg_sd();
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
        lv_image_cache_drop(NULL);   // primeiro a cache de imagens (os ícones voltam a ler-se do cartão)
    }
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
    static uint8_t chunk[2100];   // pedaço decodificado (o host envia 2 KB de cada vez)
    int32_t off = jint(msg, "off", 0);
    if (s_ntf_borrowed) {   // a memória do fundo está com o aviso: o host tenta de novo depois
        bg_ack(off, false);
        return;
    }
    if (off == 0) {
        s_bg_next = 0;
        int32_t w = jint(msg, "w", 0), h = jint(msg, "h", 0);
        uint8_t bpp = jint(msg, "bpp", 4) == 8 ? 8 : 4;
        size_t need = bpp == 8 ? (size_t)w * h : ((size_t)w * h + 1) / 2;
        // o relógio lê o fundo num timer do LVGL: a troca do buffer é feita com o lock
        if (!lvgl_port_lock(1000)) {
            bg_ack(off, false);
            return;
        }
        s_bg_ready = false;
        if (s_bg_fw) {
            fclose(s_bg_fw);
            s_bg_fw = NULL;
        }
        if (w <= 0 || h <= 0 || w * h > PLOT_MAX_PX) {
            lvgl_port_unlock();
            return;
        }
        // na RAM se couber (mais rápido); senão no cartão SD
        if (need != s_bg_size || s_bg_on_sd || !s_bg) {
            free(s_bg);
            s_bg = NULL;
            s_bg_size = 0;
            s_bg_on_sd = false;
            // na RAM (o relógio repinta o fundo a cada quadro; ler do cartão leva 120+ ms por quadro);
            // o cartão só é usado se não houver RAM
            if (bpp == 8) make_room(need);
            s_bg = heap_caps_malloc(need, MALLOC_CAP_8BIT);
            if (!s_bg && s_sd_ok) {
                s_bg_fw = fopen(BG_FILE, "wb");
                s_bg_on_sd = s_bg_fw != NULL;
                ESP_LOGI(TAG, "fundo de %u B no cartão (livre %u, maior bloco %u)", (unsigned)need,
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
            s_bg_size = (s_bg || s_bg_on_sd) ? need : 0;
            if (!s_bg_size) {
                ESP_LOGE(TAG, "sem memória para fundo (%u B, livre %u)", (unsigned)need,
                         (unsigned)esp_get_free_heap_size());
                lvgl_port_unlock();
                bg_ack(off, false);
                return;
            }
        } else if (s_bg_on_sd) {
            s_bg_fw = fopen(BG_FILE, "wb");
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
    bool have = s_bg_on_sd ? s_bg_fw != NULL : s_bg != NULL;
    if (!have || off < 0 || (size_t)off != s_bg_next || (size_t)off >= s_bg_size) {
        bg_ack(off, false);
        return;
    }
    const char *d = jstr(msg, "d", "");
    size_t olen = 0;
    uint8_t *dst = s_bg_on_sd ? chunk : s_bg + off;
    size_t room = s_bg_on_sd ? sizeof(chunk) : s_bg_size - off;
    if (mbedtls_base64_decode(dst, room, &olen, (const unsigned char *)d, strlen(d)) != 0 ||
        (s_bg_on_sd && fwrite(chunk, 1, olen, s_bg_fw) != olen)) {
        ESP_LOGW(TAG, "bg: pedaço inválido em %ld", (long)off);
        bg_ack(off, false);
        return;
    }
    s_bg_next = off + olen;
    if (jbool(msg, "last", false)) {
        if (s_bg_fw) {
            fclose(s_bg_fw);
            s_bg_fw = NULL;
        }
        s_bg_ready = s_bg_next == s_bg_size || s_bg_next + 1 == s_bg_size;
    }
    bg_ack(off, true);
}

// ---------- imagem JPEG no canvas (pré-visualização da câmara) ----------
// {"t":"jpg","id":screen,"off":n,"size":total,"d":"base64","last":bool}: o host envia um JPEG pequeno
// (já do tamanho do canvas) em pedaços; no último, é descodificado com o TJpgDec da ROM direto para o
// canvas do plot desta tela, e o ESP responde {"t":"jpgack","ok":bool,"ms":tempo}. O host só envia o
// quadro seguinte depois da resposta, por isso a cadência ajusta-se sozinha.
#define JPG_MAX (24 * 1024)
#define JPG_POOL 3200
static uint8_t *s_jpg;          // JPEG a receber (libertado ao mudar de tela)
static size_t s_jpg_len, s_jpg_cap, s_jpg_pos;
static int32_t s_jpg_ox, s_jpg_oy;

static void jpg_ack(bool ok, int ms)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "jpgack");
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddNumberToObject(o, "ms", ms);
    link_send(o);
    cJSON_Delete(o);
}

static void jpg_free(void)
{
    free(s_jpg);
    s_jpg = NULL;
    s_jpg_cap = s_jpg_len = 0;
}

static UINT jpg_in(JDEC *jd, BYTE *buf, UINT n)
{
    (void)jd;
    if (s_jpg_pos + n > s_jpg_len) n = (UINT)(s_jpg_len - s_jpg_pos);
    if (buf) memcpy(buf, s_jpg + s_jpg_pos, n);
    s_jpg_pos += n;
    return n;
}

static UINT jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    (void)jd;
    const uint8_t *src = bitmap;   // RGB888 (formato da ROM)
    for (int y = r->top; y <= r->bottom; y++) {
        int32_t dy = y + s_jpg_oy;
        if (dy < 0 || dy >= s_ch) {
            src += 3 * (r->right - r->left + 1);
            continue;
        }
        uint16_t *row = (uint16_t *)(s_cbuf.data + dy * s_cbuf.header.stride);
        for (int x = r->left; x <= r->right; x++, src += 3) {
            int32_t dx = x + s_jpg_ox;
            if (dx >= 0 && dx < s_cw) {
                row[dx] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
            }
        }
    }
    return 1;
}

static void handle_jpg(const cJSON *msg)
{
    int32_t off = jint(msg, "off", 0);
    if (off == 0) {
        int32_t size = jint(msg, "size", 0);
        if (size <= 0 || size > JPG_MAX) {
            jpg_ack(false, 0);
            return;
        }
        if ((size_t)size > s_jpg_cap) {
            jpg_free();
            s_jpg = heap_caps_malloc(size, MALLOC_CAP_8BIT);
            if (!s_jpg) {
                ESP_LOGW(TAG, "jpg: sem memória para %ld B", (long)size);
                jpg_ack(false, 0);
                return;
            }
            s_jpg_cap = size;
        }
        s_jpg_len = size;
        s_jpg_pos = 0;
    }
    const char *d = jstr(msg, "d", "");
    size_t olen = 0;
    if (!s_jpg || off < 0 || (size_t)off != s_jpg_pos || (size_t)off >= s_jpg_len ||
        mbedtls_base64_decode(s_jpg + off, s_jpg_len - off, &olen, (const unsigned char *)d, strlen(d)) != 0) {
        s_jpg_pos = 0;
        jpg_ack(false, 0);
        return;
    }
    s_jpg_pos = off + olen;
    if (!jbool(msg, "last", false)) return;   // os pedaços do meio não têm resposta
    bool ok = false;
    int64_t t0 = esp_timer_get_time();
    void *pool = malloc(JPG_POOL);
    if (pool && s_jpg_pos == s_jpg_len && lvgl_port_lock(1000)) {
        if (s_tpl == TPL_PLOT && s_canvas && s_cmem && !strcmp(jstr(msg, "id", ""), s_screen_id)) {
            JDEC jd;
            s_jpg_len = s_jpg_pos;
            s_jpg_pos = 0;
            if (jd_prepare(&jd, jpg_in, pool, JPG_POOL, NULL) == JDR_OK) {
                uint8_t sc = 0;   // reduz 1/2, 1/4 ou 1/8 se a imagem for maior que o canvas
                while (sc < 3 && ((int32_t)(jd.width >> sc) > s_cw || (int32_t)(jd.height >> sc) > s_ch)) sc++;
                s_jpg_ox = (s_cw - (int32_t)(jd.width >> sc)) / 2;
                s_jpg_oy = (s_ch - (int32_t)(jd.height >> sc)) / 2;
                ok = jd_decomp(&jd, jpg_out, sc) == JDR_OK;
                lv_obj_invalidate(s_canvas);
            }
        }
        lvgl_port_unlock();
    }
    free(pool);
    s_jpg_pos = 0;
    jpg_ack(ok, (int)((esp_timer_get_time() - t0) / 1000));
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
    draw_icons(cJSON_GetObjectItemCaseSensitive(p, "pts"));
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
        bool icon = cJSON_IsString(cJSON_GetArrayItem(pt, 6));   // já desenhado por draw_icons
        if (icon) {
            if (cJSON_IsString(jl) && jl->valuestring[0]) {
                bool left = px > w - 45;
                plot_text(&layer, jl->valuestring, left ? px - 9 : px + 9, py - 7, 50, col,
                          left ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
            }
            continue;
        }
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

    // textos soltos: [x, y, cor, texto, "l|c|r"] (y = meio da linha), ex.: horas e valores dos eixos
    const cJSON *txs = cJSON_GetObjectItemCaseSensitive(p, "texts");
    const cJSON *tx;
    cJSON_ArrayForEach(tx, txs) {
        const cJSON *jx = cJSON_GetArrayItem(tx, 0), *jy = cJSON_GetArrayItem(tx, 1);
        const cJSON *jt = cJSON_GetArrayItem(tx, 3);
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) || !cJSON_IsString(jt)) continue;
        const cJSON *jc = cJSON_GetArrayItem(tx, 2), *ja = cJSON_GetArrayItem(tx, 4);
        const char *al = cJSON_IsString(ja) ? ja->valuestring : "l";
        plot_text(&layer, jt->valuestring, (int32_t)jx->valuedouble, (int32_t)jy->valuedouble, 80,
                  parse_color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_SUB),
                  al[0] == 'c' ? LV_TEXT_ALIGN_CENTER : al[0] == 'r' ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
    }

    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(p, "tag");
    if (cJSON_IsArray(tag)) draw_tag(&layer, tag, w, h);

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

// ---------- ícones dos pontos do mapa (7.º campo de "pts": "plane", "ship" ou "balloon") ----------
// Escritos direto no buffer RGB565, como os trajetos (por cima deles e por baixo dos rótulos).
// Triângulos em 1/2 px com o nariz para cima, rodados pelo rumo; contorno escuro de 1 px para se verem
// em cima do mapa. O balão não roda.
typedef struct { int8_t x0, y0, x1, y1, x2, y2; } tri8_t;
static const tri8_t ICON_PLANE[] = {
    {0, -16, -3, -9, 3, -9},                          // nariz
    {-3, -9, 3, -9, 3, 10}, {-3, -9, 3, 10, -3, 10},  // fuselagem
    {0, -7, -17, 5, 17, 5},                           // asas
    {0, 5, -8, 14, 8, 14},                            // cauda
};
static const tri8_t ICON_SHIP[] = {
    {0, -18, -7, -6, 7, -6},                          // proa
    {-7, -6, 7, -6, 6, 15}, {-7, -6, 6, 15, -6, 15},  // casco (popa um pouco mais estreita)
};
static const tri8_t ICON_BALLOON_NECK[] = {{-4, -12, 4, -12, 0, -6}};

static void fill_tri16(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t c)
{
    // coordenadas em 1/16 px; amostra no centro de cada píxel (só contas inteiras: o C6 não tem FPU)
    int32_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (!area) return;
    int32_t mnx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t mxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t mny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t mxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int32_t ax = mnx >> 4, bx = (mxx >> 4) + 1, ay = mny >> 4, by = (mxy >> 4) + 1;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (bx >= s_cw) bx = s_cw - 1;
    if (by >= s_ch) by = s_ch - 1;
    for (int32_t py = ay; py <= by; py++) {
        int32_t sy = py * 16 + 8;
        uint16_t *row = (uint16_t *)(s_cbuf.data + py * s_cbuf.header.stride);
        for (int32_t px = ax; px <= bx; px++) {
            int32_t sx = px * 16 + 8;
            int32_t w0 = (x1 - x0) * (sy - y0) - (y1 - y0) * (sx - x0);
            int32_t w1 = (x2 - x1) * (sy - y1) - (y2 - y1) * (sx - x1);
            int32_t w2 = (x0 - x2) * (sy - y2) - (y0 - y2) * (sx - x2);
            if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) row[px] = c;
        }
    }
}

static void fill_circle(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    for (int32_t y = -r; y <= r; y++) {
        for (int32_t x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r + r) px_put(cx + x, cy + y, c);
        }
    }
}

static void draw_tris(const tri8_t *t, int n, int32_t px, int32_t py, int32_t sn, int32_t cs, uint16_t c)
{
    // sn, cs: seno e cosseno do rumo x 1024; vértices em 1/2 px -> 1/16 px
    for (int i = 0; i < n; i++) {
        int32_t v[6] = {t[i].x0, t[i].y0, t[i].x1, t[i].y1, t[i].x2, t[i].y2}, o[6];
        for (int k = 0; k < 6; k += 2) {
            o[k] = px * 16 + 8 + ((v[k] * cs - v[k + 1] * sn) * 8) / 1024;
            o[k + 1] = py * 16 + 8 + ((v[k] * sn + v[k + 1] * cs) * 8) / 1024;
        }
        fill_tri16(o[0], o[1], o[2], o[3], o[4], o[5], c);
    }
}

static void draw_icon(const char *shape, int32_t px, int32_t py, const cJSON *jh, uint16_t c)
{
    static const int8_t OFF[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    const uint16_t dark = rgb565(0x05070A);
    if (!strcmp(shape, "balloon")) {
        for (int pass = 0; pass < 2; pass++) {
            uint16_t col = pass ? c : dark;
            int e = pass ? 0 : 1;
            fill_circle(px, py - 11, 5 + e, col);
            draw_tris(ICON_BALLOON_NECK, 1, px, py, 0, 1024, col);
            for (int32_t y = py - 5 - e; y <= py - 2 + e; y++) {   // fio
                for (int32_t x = px - e; x <= px + e; x++) px_put(x, y, col);
            }
            for (int32_t y = py - 1 - e; y <= py + 1 + e; y++) {   // carga
                for (int32_t x = px - 1 - e; x <= px + 1 + e; x++) px_put(x, y, col);
            }
        }
        return;
    }
    const tri8_t *t = !strcmp(shape, "ship") ? ICON_SHIP : ICON_PLANE;
    int n = t == ICON_SHIP ? (int)(sizeof(ICON_SHIP) / sizeof(*t)) : (int)(sizeof(ICON_PLANE) / sizeof(*t));
    int32_t sn = 0, cs = 1024;
    if (cJSON_IsNumber(jh)) {
        float rad = (float)jh->valuedouble * (float)M_PI / 180.0f;
        sn = (int32_t)lroundf(1024 * sinf(rad));
        cs = (int32_t)lroundf(1024 * cosf(rad));
    }
    for (int k = 0; k < 8; k++) draw_tris(t, n, px + OFF[k][0], py + OFF[k][1], sn, cs, dark);
    draw_tris(t, n, px, py, sn, cs, c);
}

static void draw_icons(const cJSON *pts)
{
    const cJSON *pt;
    int n = 0;
    cJSON_ArrayForEach(pt, pts) {
        if (n++ >= PLOT_PTS) break;
        const cJSON *js = cJSON_GetArrayItem(pt, 6), *jx = cJSON_GetArrayItem(pt, 0), *jy = cJSON_GetArrayItem(pt, 1);
        if (!cJSON_IsString(js) || !cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) continue;
        const cJSON *jc = cJSON_GetArrayItem(pt, 2);
        lv_color_t col = parse_color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_TEXT);
        draw_icon(js->valuestring, (int32_t)jx->valuedouble, (int32_t)jy->valuedouble, cJSON_GetArrayItem(pt, 4),
                  lv_color_to_u16(col));
    }
}

// Etiqueta do alvo seguido: "tag":[x,y,"#cor","linha 1\nlinha 2.."] (até 4 linhas, a 1.ª na cor do alvo).
// Uma caixa ao lado do ponto (do lado com espaço), ligada a ele por um traço.
static void draw_tag(lv_layer_t *layer, const cJSON *tag, int32_t w, int32_t h)
{
    const cJSON *jx = cJSON_GetArrayItem(tag, 0), *jy = cJSON_GetArrayItem(tag, 1);
    const cJSON *jc = cJSON_GetArrayItem(tag, 2), *jt = cJSON_GetArrayItem(tag, 3);
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) || !cJSON_IsString(jt)) return;
    int32_t x = (int32_t)jx->valuedouble, y = (int32_t)jy->valuedouble;
    lv_color_t col = parse_color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_TEXT);
    char buf[160];
    strlcpy(buf, jt->valuestring, sizeof(buf));
    char *ln[4];
    int n = 0;
    for (char *p = buf; n < 4 && p; n++) {
        ln[n] = p;
        p = strchr(p, '\n');
        if (p) *p++ = 0;
    }
    int32_t tw = 0;
    for (int i = 0; i < n; i++) {
        int32_t lw = lv_text_get_width(ln[i], strlen(ln[i]), &dash_font_12, 0);
        if (lw > tw) tw = lw;
    }
    int32_t bw = tw + 10, bh = n * 14 + 6;
    if (bw > w - 4) bw = w - 4;
    // 6 posições à volta do ponto (direita/esquerda; acima, ao lado, abaixo): a 1.ª que cabe no canvas sem
    // tapar os botões do mapa (zoom, Centrar, Soltar); se nenhuma servir, a 1.ª ajustada às bordas
    lv_area_t btn[CTRL_MAX];
    int nb = 0;
    if (s_plot_wrap && s_canvas) {
        lv_area_t ca;
        lv_obj_get_coords(s_canvas, &ca);
        for (uint32_t i = 1; i < lv_obj_get_child_count(s_plot_wrap) && nb < CTRL_MAX; i++) {
            lv_area_t a;
            lv_obj_get_coords(lv_obj_get_child(s_plot_wrap, i), &a);
            btn[nb++] = (lv_area_t){a.x1 - ca.x1 - 3, a.y1 - ca.y1 - 3, a.x2 - ca.x1 + 3, a.y2 - ca.y1 + 3};
        }
    }
    const int32_t cand[6][2] = {{x + 12, y - 12 - bh}, {x - 12 - bw, y - 12 - bh}, {x + 12, y - bh / 2},
                                {x - 12 - bw, y - bh / 2}, {x + 12, y + 12}, {x - 12 - bw, y + 12}};
    int32_t bx = cand[0][0], by = cand[0][1];
    for (int k = 0; k < 6; k++) {
        int32_t cx0 = cand[k][0], cy0 = cand[k][1], cx1 = cx0 + bw, cy1 = cy0 + bh;
        if (cx0 < 2 || cy0 < 2 || cx1 > w - 2 || cy1 > h - 2) continue;
        bool hit = false;
        for (int b = 0; b < nb && !hit; b++)
            hit = cx0 < btn[b].x2 && cx1 > btn[b].x1 && cy0 < btn[b].y2 && cy1 > btn[b].y1;
        if (hit) continue;
        bx = cx0;
        by = cy0;
        goto placed;
    }
    if (bx + bw > w - 2) bx = x - 12 - bw;
    if (bx < 2) bx = 2;
    if (by < 2) by = y + 12;
    if (by + bh > h - 2) by = h - 2 - bh;
placed:;
    int32_t lx = bx > x ? bx : bx + bw, ly = by > y ? by : by + bh;
    plot_line(layer, x, y, lx, ly, col, 1);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(C_BAR);
    rd.bg_opa = LV_OPA_80;
    rd.radius = 4;
    rd.border_color = col;
    rd.border_width = 1;
    lv_area_t ar = {bx, by, bx + bw - 1, by + bh - 1};
    lv_draw_rect(layer, &rd, &ar);
    for (int i = 0; i < n; i++) {
        plot_text(layer, ln[i], bx + 5, by + 3 + 7 + i * 14, bw - 8, i ? lv_color_hex(C_TEXT) : col,
                  LV_TEXT_ALIGN_LEFT);
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
        const cJSON *jc = cJSON_GetArrayItem(c, 3);   // cor do fundo (opcional)
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
        // rótulos de 1 carácter (zoom + e −) num botão grande, fácil de acertar com o dedo
        bool big = lbl && strlen(lbl) <= 3 && lv_text_get_width(lbl, strlen(lbl), &dash_font_16, 0) < 20;
        lv_obj_set_size(b, LV_SIZE_CONTENT, big ? 38 : 30);   // largura do rótulo, no mínimo 30 (46 nos grandes)
        lv_obj_set_style_min_width(b, big ? 46 : 30, 0);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_align(b, al, 0, 0);
        lv_obj_set_style_radius(b, 7, 0);
        lv_obj_set_style_bg_color(b, parse_color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_BAR), 0);
        lv_obj_set_style_bg_opa(b, cJSON_IsString(jc) ? LV_OPA_90 : LV_OPA_70, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_CARD_PR), 0);
        lv_obj_set_ext_click_area(b, big ? 8 : 4);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);   // tocar numa seta não é swipe
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, ctrl_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mk_label(b, big ? &dash_font_20 : &dash_font_16, C_TEXT, lbl ? lbl : sym);
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

static void pet_bg_save(void)
{
    int r[4];
    s_pet_ok = false;
    if (!clockface_pet_rect(&s_cs, &r[0], &r[1], &r[2], &r[3])) return;
    if (r[1] < 0) { r[3] += r[1]; r[1] = 0; }
    if (r[0] + r[2] > s_cw) r[2] = s_cw - r[0];
    if (r[1] + r[3] > s_ch) r[3] = s_ch - r[1];
    if (r[2] <= 0 || r[3] <= 0) return;
    if (!s_pet_bg || r[2] * r[3] != s_pet_r[2] * s_pet_r[3]) {
        free(s_pet_bg);
        s_pet_bg = malloc((size_t)r[2] * r[3] * 2);
        if (!s_pet_bg) return;
    }
    memcpy(s_pet_r, r, sizeof(r));
    for (int j = 0; j < r[3]; j++)
        memcpy(s_pet_bg + j * r[2], s_cbuf.data + (r[1] + j) * s_cbuf.header.stride + r[0] * 2, (size_t)r[2] * 2);
    s_pet_ok = true;
}

// Um quadro do cão: repõe o fundo da faixa, desenha o cão e manda só essa zona para o ecrã.
static void clock_pet_frame(void)
{
    if (!s_pet_ok || !s_canvas || !s_cmem) return;
    const int *r = s_pet_r;
    for (int j = 0; j < r[3]; j++)
        memcpy(s_cbuf.data + (r[1] + j) * s_cbuf.header.stride + r[0] * 2, s_pet_bg + j * r[2], (size_t)r[2] * 2);
    clockface_draw_pet((uint16_t *)s_cbuf.data, (int)(s_cbuf.header.stride / 2), s_cw, s_ch, &s_cs, now_ms());
    lv_area_t a, co;
    lv_obj_get_coords(s_canvas, &co);
    a.x1 = co.x1 + r[0];
    a.y1 = co.y1 + r[1];
    a.x2 = a.x1 + r[2] - 1;
    a.y2 = a.y1 + r[3] - 1;
    lv_obj_invalidate_area(s_canvas, &a);
    s_pet_frames++;
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
    s_cs.now = s_has_time ? (esp_timer_get_time() + s_epoch_off) / 1000000 : 0;
    if (CLK_HAS_SCENE(s_cs.face)) {   // paisagem, previsão e cidade têm fundo do host
        bool ok = s_bg_ready && !strncmp(s_bg_key, "clk", 3) && s_bg_w == s_cw && s_bg_h == s_ch;
        if (ok) {
            paint_bg(s_bg_key);
            s_clock_drawn_bg = true;
            pet_bg_save();
        } else if (s_clock_drawn_bg && !s_bg_ready) {
            return;   // fundo novo a chegar (~1 s): mantém a última imagem
        } else {
            lv_canvas_fill_bg(s_canvas, lv_color_hex(0x101830), LV_OPA_COVER);
            s_pet_ok = false;
        }
    }
    int64_t t0 = esp_timer_get_time();
    clockface_draw((uint16_t *)s_cbuf.data, (int)(s_cbuf.header.stride / 2), s_cw, s_ch, &s_cs, &tm, ms);
    int64_t dt = esp_timer_get_time() - t0;
    s_clk_dsum += dt;
    s_clk_dn++;
    if (dt > s_clk_dmax) s_clk_dmax = dt;
    lv_obj_invalidate(s_canvas);
    s_clock_last_sec = s_cs.has_time ? tm.tm_sec : -1;
    s_clock_full_ms = ms;
}


static void clock_timer_cb(lv_timer_t *t)
{
    if (!s_clock) return;
    struct tm tm;
    bool ht = clock_tm(&tm);
    uint32_t ms = now_ms();
    bool ovl = (int32_t)(s_bright_until - ms) > 0 || (int32_t)(s_dots_until - ms) > 0 ||
               (int32_t)(s_exit_until - ms) > 0;
    bool bg_new = CLK_HAS_SCENE(s_cs.face) && s_bg_ready && !s_clock_drawn_bg;
    // chuva/neve a ~8 qps no ecrã inteiro; o cão a ~14 qps só na faixa onde corre (o fundo tem de estar
    // na RAM: do cartão seria lento demais)
    bool anim = clockface_animated(&s_cs) && ms - s_clock_full_ms >= 120;
    if (anim || (ht && tm.tm_sec != s_clock_last_sec) || ovl || ovl != s_ovl_was || bg_new) {
        clock_draw();
    } else if (s_cs.face == 0 && s_cs.pet == 1) {
        clock_pet_frame();
    }
    s_ovl_was = ovl;
    if (!s_dimmed && s_dim_ms && lv_display_get_inactive_time(NULL) > s_dim_ms && s_dim_bl < s_bl) {
        s_dimmed = true;
        board_set_backlight(s_dim_bl);
    }
}

static void clock_gesture(lv_dir_t d);

// Ajuste local de uma seta do temporizador/alarme (o host recebe o valor ao soltar).
static void tz_step(int z)
{
    switch (z) {
    case CZ_TMIN_UP: s_cs.tset += 60; break;
    case CZ_TMIN_DN: s_cs.tset -= 60; break;
    case CZ_TSEC_UP: s_cs.tset += 10; break;
    case CZ_TSEC_DN: s_cs.tset -= 10; break;
    case CZ_AH_UP: s_cs.alh = (s_cs.alh + 1) % 24; break;
    case CZ_AH_DN: s_cs.alh = (s_cs.alh + 23) % 24; break;
    case CZ_AM_UP: s_cs.alm = (s_cs.alm + 1) % 60; break;
    case CZ_AM_DN: s_cs.alm = (s_cs.alm + 59) % 60; break;
    default: return;
    }
    if (s_cs.tset < 0) s_cs.tset = 0;
    if (s_cs.tset > 99 * 60 + 59) s_cs.tset = 99 * 60 + 59;
    if (z <= CZ_TSEC_DN) s_edit_tm = true;
    else s_edit_al = true;
    s_edit_until = now_ms() + 2000;
}

static void tz_send(void)
{
    char v[12];
    if (s_edit_tm) {
        snprintf(v, sizeof(v), "%d", s_cs.tset);
        link_send_event("clock", "tset", v, NULL, NULL);
    }
    if (s_edit_al) {
        snprintf(v, sizeof(v), "%02d:%02d", s_cs.alh, s_cs.alm);
        link_send_event("clock", "alarm", v, NULL, NULL);
    }
    s_edit_tm = s_edit_al = false;
}

// Toque (sem arrastar) numa zona do temporizador: setas ajustam, botões agem.
static void tz_tap(int z)
{
    if (z == CZ_TGO) {
        link_send_event("clock", "tact", s_cs.tend > 0 ? "pause" : "start", NULL, NULL);
    } else if (z == CZ_TRESET) {
        link_send_event("clock", "tact", "reset", NULL, NULL);
    } else if (z == CZ_AON) {
        s_cs.alon = !s_cs.alon;
        s_edit_until = now_ms() + 2000;
        link_send_event("clock", "aon", s_cs.alon ? "1" : "0", NULL, NULL);
    } else {
        tz_step(z);
        tz_send();
    }
}

// Gestos do relógio medidos aqui (do toque ao soltar), sem o detetor do LVGL: na altura de 172 px
// um swipe vertical raramente chegava ao limite dele e era tratado como toque (saía do relógio).
// No mostrador do temporizador, os toques nas zonas são botões (manter o dedo numa seta repete);
// no digital retro, um toque longo muda a cor do LCD.
static void clock_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int32_t th = 24;
    uint32_t ms = now_ms();
    bool exit_shown = (int32_t)(s_exit_until - ms) > 0;
    if (code == LV_EVENT_PRESSED) {
        s_press_pt = p;
        s_pressing = true;
        s_long_done = false;
        s_press_zone = exit_shown ? CZ_NONE : clockface_zone(&s_cs, s_cw, s_ch, p.x, p.y);
        if (s_dimmed) {   // escurecido: o toque repõe o brilho (e segue como um toque normal)
            s_dimmed = false;
            board_set_backlight(s_bl);
        }
        if (s_press_zone) {
            s_cs.hot = s_press_zone;
            clock_draw();
        }
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        if (!s_pressing || ax >= th || ay >= th || exit_shown) return;
        if (s_cs.face == 1 && code == LV_EVENT_LONG_PRESSED) {   // digital retro: muda a cor do LCD
            char v[4];
            s_cs.pal = (s_cs.pal + 1) % 3;
            snprintf(v, sizeof(v), "%d", s_cs.pal);
            link_send_event("clock", "pal", v, NULL, NULL);
            s_long_done = true;
            clock_draw();
        } else if (s_press_zone >= CZ_TMIN_UP && s_press_zone <= CZ_AM_DN &&
                   s_press_zone != CZ_TGO && s_press_zone != CZ_TRESET) {   // setas: repete
            tz_step(s_press_zone);
            s_long_done = true;
            clock_draw();
        }
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_pressing) return;
    s_pressing = false;
    int zone = s_press_zone;
    s_press_zone = CZ_NONE;
    if (s_cs.hot) {
        s_cs.hot = CZ_NONE;
        clock_draw();
    }
    if (ax >= th && ax > ay) {
        if (s_edit_tm || s_edit_al) tz_send();
        clock_gesture(dx < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT);
    } else if (ay >= th) {
        if (s_edit_tm || s_edit_al) tz_send();
        clock_gesture(dy < 0 ? LV_DIR_TOP : LV_DIR_BOTTOM);
    } else if (exit_shown) {   // 2.º toque dentro do aviso: sai
        s_exit_until = ms;
        go_back();
    } else if (s_long_done) {
        if (s_edit_tm || s_edit_al) tz_send();
    } else if (zone) {
        tz_tap(zone);
        clock_draw();
    } else {
        s_exit_until = ms + 2500;
        clock_draw();
    }
}

static int32_t jint_at(const cJSON *arr, int i, int32_t def)
{
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : def;
}

static void clock_apply(const cJSON *c)
{
    if (!cJSON_IsObject(c)) return;
    s_cs.face = (int)(jint(c, "face", s_cs.face) % CLK_FACES);
    s_cs.pal = jint(c, "pal", s_cs.pal);
    s_cs.night = jbool(c, "night", false);
    int dim = jint(c, "dim", -1), dim_bl = jint(c, "dim_bl", -1);
    if (dim >= 0) s_dim_ms = (uint32_t)dim * 1000;
    if (dim_bl >= 0 && dim_bl <= 100) s_dim_bl = (uint8_t)dim_bl;
    const char *an = jstr(c, "anim", "");
    s_cs.anim = !strcmp(an, "rain") ? CLK_ANIM_RAIN : !strcmp(an, "storm") ? CLK_ANIM_STORM
              : !strcmp(an, "snow") ? CLK_ANIM_SNOW : CLK_ANIM_NONE;
    strlcpy(s_clock_bg, jstr(c, "bg", ""), sizeof(s_clock_bg));
    const cJSON *pj = cJSON_GetObjectItemCaseSensitive(c, "pet");   // [x casa, base, luz 0..100, 1 brinca|0 dorme]
    if (cJSON_IsArray(pj) && cJSON_GetArraySize(pj) >= 4) {
        s_cs.pet_x = jint_at(pj, 0, 0);
        s_cs.pet_y = jint_at(pj, 1, 0);
        s_cs.pet_l = jint_at(pj, 2, 100);
        s_cs.pet = jint_at(pj, 3, 0) ? 1 : 0;
    } else {
        s_cs.pet = -1;
    }
    // temporizador e alarme: o estado a contar vem sempre do host; os valores ajustados no ESP há menos
    // de 2 s ficam (um update pode ter saído antes de o host receber o ajuste)
    const cJSON *tj = cJSON_GetObjectItemCaseSensitive(c, "timer"), *aj = cJSON_GetObjectItemCaseSensitive(c, "alarm");
    bool editing = (int32_t)(s_edit_until - now_ms()) > 0 || s_pressing;
    if (cJSON_IsObject(tj)) {
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(tj, "end");
        s_cs.tend = cJSON_IsNumber(en) ? (int64_t)en->valuedouble : 0;
        s_cs.tleft = jint(tj, "left", 0);
        if (!editing) s_cs.tset = jint(tj, "set", s_cs.tset);
    }
    if (cJSON_IsObject(aj) && !editing) {
        s_cs.alh = jint(aj, "h", s_cs.alh) % 24;
        s_cs.alm = jint(aj, "m", s_cs.alm) % 60;
        s_cs.alon = jbool(aj, "on", false);
    }
    const cJSON *wx = cJSON_GetObjectItemCaseSensitive(c, "wx");
    s_cs.has_wx = cJSON_IsObject(wx);
    if (s_cs.has_wx) {
        s_cs.temp = jint(wx, "t", 0);
        s_cs.tmax = jint(wx, "max", 0);
        s_cs.tmin = jint(wx, "min", 0);
        s_cs.rain = jint(wx, "rain", 0);
        s_cs.wind = jint(wx, "wind", 0);
    }
    // previsão por horas (mostrador 2): "hours":[[hora|-1, ícone, temp, %chuva],..], "hicons":[{pal,d},..]
    const cJSON *hs = cJSON_GetObjectItemCaseSensitive(c, "hours");
    s_cs.nh = 0;
    const cJSON *hh;
    cJSON_ArrayForEach(hh, hs) {
        if (s_cs.nh >= 6) break;
        s_cs.hours[s_cs.nh].hour = (int8_t)jint_at(hh, 0, -1);
        s_cs.hours[s_cs.nh].icon = (int8_t)jint_at(hh, 1, -1);
        s_cs.hours[s_cs.nh].temp = (int8_t)jint_at(hh, 2, 0);
        s_cs.hours[s_cs.nh].rain = (uint8_t)jint_at(hh, 3, 0);
        s_cs.nh++;
    }
    const cJSON *hic = cJSON_GetObjectItemCaseSensitive(c, "hicons");
    s_cs.nhicons = 0;
    const cJSON *ho;
    cJSON_ArrayForEach(ho, hic) {
        if (s_cs.nhicons >= 6) break;
        const char *hd = jstr(ho, "d", "");
        if (strlen(hd) != 256) continue;
        int k = s_cs.nhicons++;
        const cJSON *pal = cJSON_GetObjectItemCaseSensitive(ho, "pal");
        for (int i = 0; i < 7; i++) {
            const cJSON *pp = cJSON_GetArrayItem(pal, i);
            s_cs.hpal[k][i + 1] = (cJSON_IsString(pp) && pp->valuestring[0] == '#')
                                      ? rgb565((uint32_t)strtoul(pp->valuestring + 1, NULL, 16)) : 0xFFFF;
        }
        for (int i = 0; i < 256; i++) s_cs.hicon[k][i] = (hd[i] >= '0' && hd[i] <= '7') ? (uint8_t)(hd[i] - '0') : 0;
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
        lv_obj_add_event_cb(s_clock, clock_press_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(s_clock, clock_press_cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
        s_exit_until = now_ms();
        s_pressing = false;
        s_dimmed = false;
        lv_display_trigger_activity(NULL);
        s_canvas = lv_canvas_create(s_clock);
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        if (!s_arena && s_cmem && (s_cw != s_W || s_ch != s_H)) {   // sem arena: liberta o buffer antigo
            free(s_cmem);
            s_cmem = NULL;
            s_cw = s_ch = 0;
        }
        if (!s_arena && !s_cmem) make_room((size_t)lv_draw_buf_width_to_stride(s_W, LV_COLOR_FORMAT_RGB565) * s_H);
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
        s_clock_timer = lv_timer_create(clock_timer_cb, 70, NULL);
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
    // devolve a memória (o host volta a enviar ícones e fundos quando precisar); a arena fica
    if (s_cmem != s_arena) free(s_cmem);
    s_cmem = NULL;
    s_cw = s_ch = 0;
    free(s_bg);
    s_bg = NULL;
    s_bg_size = 0;
    s_bg_on_sd = false;
    free(s_pet_bg);
    s_pet_bg = NULL;
    s_pet_ok = false;
    s_bg_ready = false;
    s_bg_key[0] = '\0';
    s_bl = s_bl_saved;
    board_set_backlight(s_bl);
}

// Gestos no relógio: lateral troca o mostrador (paisagem → retro → previsão → cidade → temporizador),
// vertical muda o brilho.
static void clock_gesture(lv_dir_t d)
{
    char v[8];
    uint32_t ms = now_ms();
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
        s_cs.face = (s_cs.face + (d == LV_DIR_LEFT ? 1 : CLK_FACES - 1)) % CLK_FACES;
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

// ---------- mapa arrastado com o dedo (plot "drag":true) ----------
// Arrastar desloca a imagem já desenhada (translate) e, ao soltar, envia {"t":"btn","btn":"pan:dx,dy"}
// para o host refazer o mapa. Um arrasto que começa junto à borda esquerda/direita muda de página
// (swipe); dois toques seguidos enviam "mapinfo" (o host mostra os dados do mapa).
#define MAP_EDGE 22
static bool s_map_drag, s_md_moving, s_md_edge;
static lv_point_t s_md_start;
static int64_t s_md_last_tap;
static lv_point_t s_md_last_pt;
static lv_timer_t *s_md_tap_timer;
static char s_md_tap_btn[24], s_md_tap_scr[ID_LEN];

static void map_tap_fire(lv_timer_t *t)
{
    (void)t;
    s_md_tap_timer = NULL;   // repeat_count 1: o LVGL apaga o timer
    if (s_canvas && s_map_drag && !strcmp(s_md_tap_scr, s_screen_id))
        link_send_event("btn", "id", s_screen_id, "btn", s_md_tap_btn);
}

static bool map_near_ctrl(lv_point_t p)
{
    // um toque perto de um botão do mapa (zoom, Centrar) que falhou o botão não conta como toque no mapa
    if (!s_plot_wrap) return false;
    for (uint32_t i = 1; i < lv_obj_get_child_count(s_plot_wrap); i++) {
        lv_area_t a;
        lv_obj_get_coords(lv_obj_get_child(s_plot_wrap, i), &a);
        if (p.x >= a.x1 - 14 && p.x <= a.x2 + 14 && p.y >= a.y1 - 14 && p.y <= a.y2 + 14) return true;
    }
    return false;
}

static void map_ev(lv_event_t *e)
{
    if (!s_map_drag || !s_canvas) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        lv_area_t a;
        lv_obj_get_coords(s_canvas, &a);
        s_md_start = p;
        s_md_moving = false;
        s_md_edge = p.x - a.x1 < MAP_EDGE || a.x2 - p.x < MAP_EDGE;
        return;
    }
    int32_t dx = p.x - s_md_start.x, dy = p.y - s_md_start.y;
    if (code == LV_EVENT_PRESSING) {
        if (!s_md_moving && (LV_ABS(dx) > 6 || LV_ABS(dy) > 6)) s_md_moving = true;
        if (s_md_moving && !s_md_edge) {
            lv_obj_set_style_translate_x(s_canvas, dx, 0);
            lv_obj_set_style_translate_y(s_canvas, dy, 0);
        }
        return;
    }
    if (code != LV_EVENT_RELEASED) {   // toque perdido: volta ao sítio
        lv_obj_set_style_translate_x(s_canvas, 0, 0);
        lv_obj_set_style_translate_y(s_canvas, 0, 0);
        return;
    }
    if (s_md_moving) {
        s_md_last_tap = 0;
        if (s_md_edge) {
            if (LV_ABS(dx) > 40 && LV_ABS(dx) > LV_ABS(dy))
                link_send_event("swipe", "id", s_screen_id, "dir", dx < 0 ? "left" : "right");
        } else {
            char b[32];
            snprintf(b, sizeof(b), "pan:%d,%d", (int)dx, (int)dy);
            link_send_event("btn", "id", s_screen_id, "btn", b);   // o translate fica até o mapa novo chegar
        }
        return;
    }
    if (map_near_ctrl(p)) {
        s_md_last_tap = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_md_last_tap && now - s_md_last_tap < 400000 && LV_ABS(p.x - s_md_last_pt.x) < 30 &&
        LV_ABS(p.y - s_md_last_pt.y) < 30) {
        s_md_last_tap = 0;
        if (s_md_tap_timer) {   // era um duplo toque: o toque simples não segue
            lv_timer_delete(s_md_tap_timer);
            s_md_tap_timer = NULL;
        }
        link_send_event("btn", "id", s_screen_id, "btn", "mapinfo");
    } else {
        // toque simples: espera para ver se vem o 2.º; depois envia "tap:x,y" (px do canvas)
        s_md_last_tap = now;
        s_md_last_pt = p;
        lv_area_t a;
        lv_obj_get_coords(s_canvas, &a);
        snprintf(s_md_tap_btn, sizeof(s_md_tap_btn), "tap:%d,%d", (int)(p.x - a.x1), (int)(p.y - a.y1));
        strlcpy(s_md_tap_scr, s_screen_id, sizeof(s_md_tap_scr));
        if (s_md_tap_timer) lv_timer_delete(s_md_tap_timer);
        s_md_tap_timer = lv_timer_create(map_tap_fire, 400, NULL);
        lv_timer_set_repeat_count(s_md_tap_timer, 1);
    }
}

static void map_drag_set(bool on)
{
    s_map_drag = on;
    if (!s_canvas) return;
    if (on) {
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);   // o arrasto não é um swipe de página
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLL_CHAIN);
    } else {
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
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
            lv_obj_add_event_cb(s_canvas, map_ev, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(s_canvas, map_ev, LV_EVENT_PRESSING, NULL);
            lv_obj_add_event_cb(s_canvas, map_ev, LV_EVENT_RELEASED, NULL);
            lv_obj_add_event_cb(s_canvas, map_ev, LV_EVENT_PRESS_LOST, NULL);
            s_ctrl_sig[0] = '\0';
            s_list = plain_box(s_body);
            int32_t rest = (s_W - 12) - pw - 6;
            lv_obj_set_size(s_list, rest >= 110 ? rest : LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(s_list, 6, 0);
        }
        const cJSON *pl = cJSON_GetObjectItemCaseSensitive(msg, "plot");
        // "live": o canvas mostra imagens "jpg"; num update só mudam os botões (não apaga a imagem)
        map_drag_set(jbool(pl, "drag", false));
        if (rebuild || !jbool(pl, "live", false)) {
            if (s_plot_wrap && pl) lv_obj_set_size(s_plot_wrap, jint(pl, "w", 160), jint(pl, "h", 160));
            draw_plot(pl);
            if (s_canvas) {   // mapa novo: o deslocamento do arrasto acabou
                lv_obj_set_style_translate_x(s_canvas, 0, 0);
                lv_obj_set_style_translate_y(s_canvas, 0, 0);
            }
        }
        if (pl) build_ctrl(cJSON_GetObjectItemCaseSensitive(pl, "ctrl"));
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
            set_text(s_log_lbl, txt);
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
    memset(s_dock_imgs, 0, sizeof(s_dock_imgs));
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (n >= DOCK_MAX) break;
        strlcpy(s_dock_ids[n], jstr(it, "id", ""), ID_LEN);
        strlcpy(s_dock_imgs[n], jstr(it, "img", ""), sizeof(s_dock_imgs[n]));
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
        const void *img = icon_find(jstr(it, "img", NULL));
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

// ---------- atualizações em espera durante o toque e o scroll ----------
// Uma lista que se reordena debaixo do dedo faz tocar no item errado. Enquanto o dedo está no ecrã,
// ou a tela rola (também na inércia, e até UPD_HOLD_US depois de parar), o "update" do host fica
// guardado (só o mais recente) e é aplicado depois. Um toque simples aplica-o logo ao soltar.
#define UPD_HOLD_US 1500000
static cJSON *s_upd_pending;
static int64_t s_upd_hold_until;

static bool upd_hold(void)
{
    lv_indev_t *in = lv_indev_get_next(NULL);
    int64_t now = esp_timer_get_time();
    if (in) {
        if (lv_indev_get_scroll_obj(in)) s_upd_hold_until = now + UPD_HOLD_US;
        if (lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) return true;
    }
    return now < s_upd_hold_until;
}

static void apply_update(const cJSON *msg)
{
    if (s_has_screen && !strcmp(jstr(msg, "id", ""), s_screen_id)) {
        set_pager(msg);
        apply_body(msg, false);
        const char *title = jstr(msg, "title", NULL);
        if (title) set_text(s_title, title);
    }
}

static void upd_drop(void)
{
    if (s_upd_pending) cJSON_Delete(s_upd_pending);
    s_upd_pending = NULL;
}

static void upd_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (upd_hold() || !s_upd_pending) return;
    cJSON *m = s_upd_pending;
    s_upd_pending = NULL;
    apply_update(m);
    cJSON_Delete(m);
}

static void render_screen(const cJSON *msg)
{
    s_icon_gen++;   // os ícones da tela nova passam à frente na RAM
    if (strcmp(s_screen_id, jstr(msg, "id", ""))) {
        jpg_free();
        about_end();   // saiu da tela inicial
    }
    strlcpy(s_screen_id, jstr(msg, "id", ""), ID_LEN);
    s_tpl = parse_tpl(jstr(msg, "tpl", "menu"));
    if (s_tpl != TPL_CLOCK) clock_exit();
    if (s_tpl != TPL_PLOT && s_tpl != TPL_CLOCK && s_bg && !s_ntf_borrowed) {
        // o fundo do mapa (~21 KB) não fica preso fora dos mapas: a RAM vai para os ícones da tela
        // (o host volta a enviá-lo quando um mapa abrir)
        free(s_bg);
        s_bg = NULL;
        s_bg_size = 0;
        s_bg_ready = false;
        s_bg_key[0] = '\0';
    }
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
    cJSON_AddNumberToObject(o, "largest", (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(o, "sd", board_sd_free_mb());   // MB livres no cartão, -1 sem cartão
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
static lv_obj_t *s_boot_cv;
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
    if (s_boot_mem != s_arena) free(s_boot_mem);
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
    s_boot_mem = s_arena ? s_arena : heap_caps_malloc(stride * s_H, MALLOC_CAP_8BIT);
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
static float s_ntf_total = NOTIFY_TOTAL;   // s até fechar sozinho ("dur"; o alarme fica até 60 s)

// A arena vai ser precisa para um canvas (plot, relógio): o aviso larga a animação e fica a faixa.
static void ntf_drop_canvas(void)
{
    if (s_ntf_cv) lv_obj_delete(s_ntf_cv);
    s_ntf_cv = NULL;
    s_ntf_mem = NULL;
    s_ntf_arena = 0;
}

// ---------- "Sobre o DashDeck" (toque no nome no topo da tela inicial) ----------
// Uma cena animada (aboutanim.c) a toda a largura, no buffer de ecrã reservado, com os créditos por cima:
// versões, autor e endereço do projeto, que somem ao fim de 7 s. Um toque fecha; fecha sozinha ao fim de
// ABOUT_SECS. Cada toque no
// nome mostra a outra cena (Aurora e Hiperespaço alternam).
#define ABOUT_SECS 45
#define ABOUT_CREDITS_S 7.0f   // os créditos ficam 7 s; depois só a animação
static lv_obj_t *s_about, *s_about_cv, *s_about_band;
static float s_about_until;   // s até fechar sozinha
static float s_about_cred;    // s em que os créditos aparecem (no Hiperespaço, depois do salto)
static lv_draw_buf_t s_about_buf;
static uint16_t *s_about_mem;
static lv_timer_t *s_about_timer;
static int64_t s_about_t0;
static int s_about_var;
static char s_host_ver[40];

static void about_end(void)
{
    if (s_about_timer) lv_timer_delete(s_about_timer);
    s_about_timer = NULL;
    if (s_about) lv_obj_delete(s_about);
    s_about = s_about_cv = s_about_band = NULL;
    if (s_about_mem && (void *)s_about_mem != (void *)s_arena) free(s_about_mem);
    s_about_mem = NULL;
}

static void about_timer_cb(lv_timer_t *t)
{
    (void)t;
    float el = (float)(esp_timer_get_time() - s_about_t0) / 1e6f;
    if (el > s_about_until) {
        about_end();
        return;
    }
    aboutanim_draw(s_about_mem, (int)(s_about_buf.header.stride / 2), el);
    lv_obj_invalidate(s_about_cv);
    // os créditos aparecem em s_about_cred (0,4 s) e desaparecem (0,6 s) ABOUT_CREDITS_S depois
    if (s_about_band && s_about_cred >= 0) {
        float c = el - s_about_cred;
        if (c < 0) {
        } else if (c < 0.4f) {
            lv_obj_remove_flag(s_about_band, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(s_about_band, (lv_opa_t)(255 * c / 0.4f), 0);
        } else if (c < ABOUT_CREDITS_S) {
            lv_obj_remove_flag(s_about_band, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(s_about_band, 255, 0);
        } else if (c < ABOUT_CREDITS_S + 0.6f) {
            lv_obj_set_style_opa(s_about_band, (lv_opa_t)(255 * (1 - (c - ABOUT_CREDITS_S) / 0.6f)), 0);
        } else {
            lv_obj_add_flag(s_about_band, LV_OBJ_FLAG_HIDDEN);
            s_about_cred = -1;   // acabou
        }
    }
    lv_display_trigger_activity(NULL);
}

// toque em (x, y): a cena pode usá-lo; senão fecha
static void about_tap(int32_t x, int32_t y)
{
    if (!s_about) return;
    float el = (float)(esp_timer_get_time() - s_about_t0) / 1e6f;
    if (aboutanim_touch(x, y, el)) {
        if (s_about_band) lv_obj_add_flag(s_about_band, LV_OBJ_FLAG_HIDDEN);
        s_about_cred = -1;
        s_about_until = el + 30;
        return;
    }
    about_end();
}

static void about_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_wait_release(indev);   // o toque não passa para a tela por baixo
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    about_tap(p.x, p.y);
}

static void about_show(void)
{
    about_end();
    if (s_clock || s_canvas || s_boot || s_ntf) return;   // o buffer de ecrã está ocupado
    uint32_t stride = lv_draw_buf_width_to_stride(s_W, LV_COLOR_FORMAT_RGB565);
    s_about_mem = s_arena ? (uint16_t *)s_arena : heap_caps_malloc(stride * s_H, MALLOC_CAP_8BIT);
    if (!s_about_mem) {
        show_toast("DashDeck " DASHDECK_FW_VERSION " por CT7BRA", "info");
        return;
    }
    lv_draw_buf_init(&s_about_buf, s_W, s_H, LV_COLOR_FORMAT_RGB565, stride, s_about_mem, stride * s_H);
    s_about = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_about);
    lv_obj_set_size(s_about, s_W, s_H);
    lv_obj_add_flag(s_about, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_about, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_about, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_about, about_press_cb, LV_EVENT_PRESSED, NULL);
    s_about_cv = lv_canvas_create(s_about);
    lv_obj_remove_flag(s_about_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_draw_buf(s_about_cv, &s_about_buf);
    int var = s_about_var;
    s_about_var = (s_about_var + 1) % ABOUTANIM_VARIANTS;
    aboutanim_start(var, s_W, s_H);
    aboutanim_draw(s_about_mem, (int)(stride / 2), 0);

    // créditos numa faixa escura em baixo
    lv_obj_t *band = s_about_band = lv_obj_create(s_about);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, s_W, LV_SIZE_CONTENT);
    lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(band, lv_color_hex(0x05070C), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_60, 0);
    lv_obj_set_style_pad_ver(band, 4, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(band, 1, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
    char ver[72];
    snprintf(ver, sizeof(ver), "firmware %s%s%s", DASHDECK_FW_VERSION, s_host_ver[0] ? " · host " : "", s_host_ver);
    mk_label(band, &dash_font_14, 0xFFFFFF, "por CT7BRA");
    mk_label(band, &dash_font_12, 0x7CC4FF, "github.com/tiuksferve/DashDeck");
    mk_label(band, &dash_font_12, 0xB8C2D0, ver);

    s_about_t0 = esp_timer_get_time();
    s_about_until = ABOUT_SECS;
    s_about_cred = aboutanim_credits_at(var);
    if (s_about_cred > 0) lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);   // só depois do salto
    s_about_timer = lv_timer_create(about_timer_cb, 50, NULL);
    lv_display_trigger_activity(NULL);
}

static void title_click_cb(lv_event_t *e)
{
    (void)e;
    if (!strcmp(s_screen_id, "home")) about_show();
}

static void ntf_end(void)
{
    if (s_ntf_timer) lv_timer_delete(s_ntf_timer);
    s_ntf_timer = NULL;
    if (s_ntf) lv_obj_delete(s_ntf);
    s_ntf = s_ntf_cv = s_ntf_bar = NULL;
    if (s_ntf_arena == 2) {   // a arena era do plot desta tela: o host volta a enviá-la
        if (s_canvas) lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        link_send_event("redraw", NULL, NULL, NULL, NULL);
    }
    if (!s_ntf_borrowed && !s_ntf_arena) free(s_ntf_mem);
    s_ntf_mem = NULL;
    s_ntf_borrowed = false;
    s_ntf_arena = 0;
}

static void ntf_timer_cb(lv_timer_t *t)
{
    float el = (float)(esp_timer_get_time() - s_ntf_t0) / 1e6f;
    if (s_ntf_close || el > s_ntf_total) {
        ntf_end();
        return;
    }
    float fade = el < 0.3f ? el / 0.3f : el > s_ntf_total - 0.3f ? (s_ntf_total - el) / 0.3f : 1;
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
    about_end();   // o aviso precisa do buffer de ecrã
    ntf_end();
    if (s_asleep) wake_up();
    if (s_dimmed) {   // relógio escurecido
        s_dimmed = false;
        board_set_backlight(s_bl);
    }
    lv_display_trigger_activity(NULL);
    bool alarm = !strcmp(jstr(msg, "kind", ""), "alarm");
    bool sq = !strcmp(jstr(msg, "kind", ""), "squawk");
    int dur = jint(msg, "dur", 0);
    s_ntf_total = dur > 0 && dur <= 300 ? (float)dur : NOTIFY_TOTAL;
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
    size_t largest0 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    // 1.º a arena do ecrã, se nenhum canvas (plot, relógio, arranque) a estiver a usar: não depende
    // do heap, que ao fim de horas fica fragmentado (o maior bloco livre anda perto dos 46 KB)
    if (s_arena && s_arena_size >= need && !s_canvas && !s_clock && !s_boot) {
        s_ntf_mem = s_arena;
        s_ntf_arena = 1;
    }
    if (!s_ntf_mem) s_ntf_mem = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    if (!s_ntf_mem && s_clock && s_bg && !s_bg_on_sd && s_bg_size >= need) {
        // relógio aberto: o fundo dele empresta a memória (o relógio fica parado por baixo do aviso e
        // o host volta a enviar o fundo depois: {"t":"bgfreed"})
        s_bg_ready = false;
        s_bg_key[0] = '\0';
        s_ntf_mem = s_bg;
        s_ntf_borrowed = true;
        link_send_event("bgfreed", NULL, NULL, NULL, NULL);
    }
    if (!s_ntf_mem) {
        make_room(need);
        s_ntf_mem = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    }
    if (!s_ntf_mem && s_arena && s_arena_size >= need && s_canvas && s_cmem == s_arena && !s_clock) {
        // último recurso: a arena do plot desta tela (o canvas fica escondido por baixo do aviso e o
        // host volta a desenhá-lo no fim, {"t":"redraw"})
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        s_ntf_mem = s_arena;
        s_ntf_arena = 2;
    }
    if (s_ntf_mem) {   // sem memória fica só a faixa
        lv_draw_buf_init(&s_ntf_buf, NOTIFY_W, NOTIFY_H, LV_COLOR_FORMAT_RGB565A8, NOTIFY_W * 2, s_ntf_mem, need);
        notify_start(jstr(msg, "from", ""), alarm ? NOTIFY_ALARM : sq ? NOTIFY_SQUAWK : NOTIFY_DM);
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
    const uint32_t bar_bg = alarm ? 0x3B2A00 : sq ? 0x3D0808 : 0x0E3B1C,
                   bar_fg = alarm ? 0xFFC94A : sq ? 0xFF7B72 : 0x67EA94;
    lv_obj_set_style_bg_color(s_ntf_bar, lv_color_hex(bar_bg), 0);
    lv_obj_set_style_border_side(s_ntf_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ntf_bar, 1, 0);
    lv_obj_set_style_border_color(s_ntf_bar, lv_color_hex(bar_fg), 0);
    lv_obj_set_style_pad_hor(s_ntf_bar, 8, 0);
    lv_obj_set_style_pad_ver(s_ntf_bar, 4, 0);
    lv_obj_set_flex_flow(s_ntf_bar, LV_FLEX_FLOW_COLUMN);
    char head[64];
    if (jstr(msg, "title", NULL)) utf8cpy(head, jstr(msg, "title", ""), sizeof(head));
    else snprintf(head, sizeof(head), "Mensagem de %s", jstr(msg, "from", "?"));
    lv_obj_t *l1 = mk_label(s_ntf_bar, &dash_font_14, bar_fg, head);
    lv_obj_set_width(l1, LV_PCT(100));
    lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
    lv_obj_t *l2 = mk_label(s_ntf_bar, &dash_font_12, alarm ? 0xFFF6E0 : sq ? 0xFFECEC : 0xE7FFE9,
                            jstr(msg, "text", ""));
    lv_obj_set_width(l2, LV_PCT(100));
    lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
    s_ntf_t0 = esp_timer_get_time();
    s_ntf_close = false;
    s_ntf_timer = lv_timer_create(ntf_timer_cb, 40, NULL);
    ESP_LOGI(TAG, "aviso de mensagem (%s)", s_ntf_mem ? "com animação" : "só a faixa");
    // resposta ao host: a animação teve memória? (sem ela fica só a faixa e o escurecimento)
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ntfack");
    cJSON_AddBoolToObject(o, "anim", s_ntf_mem != NULL);
    cJSON_AddBoolToObject(o, "borrowed", s_ntf_borrowed);
    cJSON_AddNumberToObject(o, "arena", s_ntf_arena);
    cJSON_AddNumberToObject(o, "largest0", (double)largest0);   // maior bloco antes do aviso
    cJSON_AddNumberToObject(o, "heap", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(o, "largest", (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddStringToObject(o, "screen", s_screen_id);
    link_send(o);
    cJSON_Delete(o);
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

// ---------- avião do cabeçalho (monitor de emergências do ADS-B) ----------
#define HDR_PW 16
#define HDR_PH 12
static const char *const HDR_PLANE[HDR_PH] = {
    "       ##       ", "       ##       ", "      ####      ", "      ####      ",
    "    ########    ", "  ############  ", "################", "###   ####   ###",
    "      ####      ", "       ##       ", "     ######     ", "    ###  ###    ",
};
static uint8_t s_hdr_px[HDR_PW * HDR_PH * 3];
static lv_image_dsc_t s_hdr_dsc;
static lv_obj_t *s_hdr_plane, *s_hdr_spacer;
static int s_hdr_state;

static void hdr_plane_draw(uint32_t color)
{
    uint16_t c = rgb565(color);
    uint16_t *px = (uint16_t *)s_hdr_px;
    uint8_t *al = s_hdr_px + HDR_PW * HDR_PH * 2;
    for (int y = 0; y < HDR_PH; y++)
        for (int x = 0; x < HDR_PW; x++) {
            bool on = HDR_PLANE[y][x] == '#';
            px[y * HDR_PW + x] = on ? c : 0;
            al[y * HDR_PW + x] = on ? 255 : 0;
        }
    s_hdr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_hdr_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    s_hdr_dsc.header.w = HDR_PW;
    s_hdr_dsc.header.h = HDR_PH;
    s_hdr_dsc.header.stride = HDR_PW * 2;
    s_hdr_dsc.data_size = sizeof(s_hdr_px);
    s_hdr_dsc.data = s_hdr_px;
}

// {"t":"hdr","plane":0|1|2}: 0 escondido, 1 laranja (monitor ligado), 2 vermelho (emergência recente).
// Na horizontal o título encolhe para não passar por baixo do avião.
static void hdr_set(int v)
{
    if (v == s_hdr_state || !s_hdr_plane) return;
    s_hdr_state = v;
    if (v) {
        hdr_plane_draw(v == 2 ? 0xF85149 : 0xF0883E);
        lv_image_cache_drop(&s_hdr_dsc);
        lv_obj_invalidate(s_hdr_plane);
        lv_obj_remove_flag(s_hdr_plane, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_hdr_plane, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_land) {
        // com o avião, o título tem largura fixa até pouco antes dele (texto curto fica à esquerda,
        // comprido acaba em "..."); sem ele, volta a ocupar o espaço livre
        lv_obj_set_flex_grow(s_title, v ? 0 : 1);
        lv_obj_set_width(s_title, v ? s_W / 2 - 46 : LV_SIZE_CONTENT);
        if (v) lv_obj_remove_flag(s_hdr_spacer, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_hdr_spacer, LV_OBJ_FLAG_HIDDEN);
    }
}

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
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_CLICKABLE);      // toque no nome: "Sobre o DashDeck"
    lv_obj_set_ext_click_area(s_title, 10);
    lv_obj_add_event_cb(s_title, title_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_grow(s_title, 1);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    // com o avião ao centro, o título encolhe e este espaçador ocupa o resto (a hora fica à direita)
    s_hdr_spacer = plain_box(bar);
    lv_obj_set_size(s_hdr_spacer, 0, 1);
    lv_obj_set_flex_grow(s_hdr_spacer, 1);
    lv_obj_add_flag(s_hdr_spacer, LV_OBJ_FLAG_HIDDEN);
    s_pager = plain_box(bar);
    lv_obj_set_size(s_pager, LV_SIZE_CONTENT, 6);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pager, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_pager, 3, 0);
    lv_obj_set_style_pad_right(s_pager, 4, 0);
    lv_obj_add_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
    // avião do monitor de emergências ({"t":"hdr"}): ao centro na horizontal, junto da hora na vertical
    s_hdr_plane = lv_image_create(bar);
    hdr_plane_draw(0xF0883E);
    lv_image_set_src(s_hdr_plane, &s_hdr_dsc);
    if (s_land) {
        lv_obj_add_flag(s_hdr_plane, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(s_hdr_plane, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_add_flag(s_hdr_plane, LV_OBJ_FLAG_HIDDEN);
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

    // cartão SD (ícones e fundos grandes) e o buffer de ecrã reservado, antes de tudo o resto
    s_sd_ok = board_sd_mount();
    if (s_sd_ok) mkdir(SD_DIR, 0775);
    s_arena_size = (size_t)lv_draw_buf_width_to_stride(s_W, LV_COLOR_FORMAT_RGB565) * s_H;
    s_arena = heap_caps_malloc(s_arena_size, MALLOC_CAP_8BIT);
    if (!s_arena) s_arena_size = 0;
    ESP_LOGI(TAG, "arena %u B, SD %s, livre %u", (unsigned)s_arena_size, s_sd_ok ? "sim" : "não",
             (unsigned)esp_get_free_heap_size());

    // a animação é a tela de carregamento; o spinner "Aguardando uConsole" só fica se faltar memória
    show_waiting();
    if (boot_start()) lv_obj_clean(s_body);
    lv_timer_create(housekeeping_cb, 500, NULL);
    lv_timer_create(button_poll_cb, 40, NULL);
    lv_timer_create(upd_timer_cb, 100, NULL);

    ts_hook();
    // Inércia do scroll: o LVGL tira esta % à velocidade a cada leitura do toque. O padrão (10) conta
    // com ~30 leituras/s; o toque daqui é lido ~100 vezes/s e o scroll parava logo ao soltar.
    lv_indev_t *tin = lv_indev_get_next(NULL);
    if (tin) lv_indev_set_scroll_throw(tin, 4);
    lvgl_port_unlock();
    send_hello();
}

// ---------- captura do ecrã ({"t":"shot"}) ----------
// Redesenha o ecrã todo de uma vez e envia ao host cada área que vai para o display, antes da troca de
// bytes do esp_lvgl_port: {"t":"shotpx","x","y","w","h","d":<RGB565 em base64>} por blocos de linhas,
// e no fim {"t":"shotend","w","h"}. O host monta o PNG (dashd: SIGUSR2 ou ficheiro de pedido).
static bool s_shot_on;
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
    for (int y = 0; y < h; y += rows) {
        int n = h - y < rows ? h - y : rows;
        int k = snprintf(s_shot_line, sizeof(s_shot_line),
                         "{\"t\":\"shotpx\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"d\":\"",
                         (int)a->x1, (int)a->y1 + y, w, n);
        // junta as linhas num bloco contínuo (o stride pode ter enchimento) e codifica de uma vez
        static uint8_t raw[1800];
        size_t rl = 0;
        for (int r = 0; r < n; r++) {
            memcpy(raw + rl, b->data + (size_t)(y + r) * stride, (size_t)w * 2);
            rl += (size_t)w * 2;
        }
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char *)s_shot_line + k, sizeof(s_shot_line) - k - 4, &olen, raw, rl);
        k += (int)olen;
        k += snprintf(s_shot_line + k, sizeof(s_shot_line) - k, "\"}");
        link_send_line(s_shot_line, (size_t)k);
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
    s_shot_on = true;
    lv_refr_now(d);
    s_shot_on = false;
    char buf[64];
    int k = snprintf(buf, sizeof(buf), "{\"t\":\"shotend\",\"w\":%d,\"h\":%d}", (int)s_W, (int)s_H);
    link_send_line(buf, (size_t)k);
}

// ---------- medição do desenho ({"t":"bench"}) ----------
// Rola a tela atual para os lados (ida e volta, 2 s) e mede cada atualização do ecrã: quantas houve,
// a média e o pior tempo (desenho + envio ao display). Responde {"t":"bench","frames",..}.
static int64_t s_bn_t0, s_bn_start, s_bn_sum, s_bn_max;
static int s_bn_frames;
static bool s_bn_on;

// Estatísticas com o dedo no ecrã ({"t":"stats"} devolve e zera): leituras do toque por segundo e
// tempo de cada atualização do ecrã enquanto há toque. Servem para ver se a lentidão vem do toque.
static lv_indev_read_cb_t s_ts_orig;
static int64_t s_ts_down, s_ts_time, s_ts_last, s_ts_gap_max;
static int s_ts_reads, s_ts_presses, s_ts_frames;
static int64_t s_ts_fsum, s_ts_fmax, s_ts_f0;
static bool s_ts_pressed;
static int64_t s_lock_max;      // maior tempo de uma mensagem com o LVGL bloqueado
static char s_lock_type[12];

static void ts_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_ts_orig(indev, data);
    int64_t now = esp_timer_get_time();
    bool p = data->state == LV_INDEV_STATE_PRESSED;
    if (p && !s_ts_pressed) {
        s_ts_down = now;
        s_ts_presses++;
    } else if (p) {
        if (now - s_ts_last > s_ts_gap_max) s_ts_gap_max = now - s_ts_last;
    } else if (s_ts_pressed) {
        s_ts_time += now - s_ts_down;
    }
    if (p) {
        s_ts_reads++;
        s_ts_last = now;
    }
    s_ts_pressed = p;
}

static void ts_frame(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_ts_f0 = s_ts_pressed ? esp_timer_get_time() : 0;
    } else if (s_ts_f0) {
        int64_t d = esp_timer_get_time() - s_ts_f0;
        s_ts_f0 = 0;
        s_ts_frames++;
        s_ts_fsum += d;
        if (d > s_ts_fmax) s_ts_fmax = d;
    }
}

static void ts_hook(void)
{
    lv_indev_t *in = lv_indev_get_next(NULL);
    if (!in || s_ts_orig) return;
    s_ts_orig = lv_indev_get_read_cb(in);
    lv_indev_set_read_cb(in, ts_read);
    lv_display_add_event_cb(lv_display_get_default(), ts_frame, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(lv_display_get_default(), ts_frame, LV_EVENT_REFR_READY, NULL);
}

static void ts_report(void)
{
    double secs = s_ts_time / 1e6;
    int ic_ram = 0, ic_sd = 0, em = 0;
    size_t em_b = 0;
    for (int i = 0; i < ICON_MAX; i++) {
        if (s_icons[i].data) ic_ram++;
        else if (s_icons[i].path[0]) ic_sd++;
    }
    for (int i = 0; i < EMOJI_MAX; i++) {
        if (s_emoji[i].data) {
            em++;
            em_b += s_emoji[i].dsc.data_size;
        }
    }
    char buf[460];
    snprintf(buf, sizeof(buf), "{\"t\":\"stats\",\"presses\":%d,\"press_s\":%.2f,\"reads_per_s\":%.1f,"
             "\"gap_max_ms\":%.1f,\"frames\":%d,\"frame_avg_ms\":%.1f,\"frame_max_ms\":%.1f,"
             "\"lock_max_ms\":%.1f,\"lock_type\":\"%s\",\"clk_draws\":%d,\"clk_avg_ms\":%.1f,"
             "\"clk_max_ms\":%.1f,\"heap\":%u,\"largest\":%u,\"icons_ram\":%d,\"icons_sd\":%d,"
             "\"emojis\":%d,\"emoji_kb\":%u,\"bg_sd\":%d,\"pet_frames\":%d}",
             s_ts_presses, secs, secs > 0 ? s_ts_reads / secs : 0.0, s_ts_gap_max / 1000.0, s_ts_frames,
             s_ts_frames ? s_ts_fsum / 1000.0 / s_ts_frames : 0.0, s_ts_fmax / 1000.0, s_lock_max / 1000.0,
             s_lock_type, s_clk_dn, s_clk_dn ? s_clk_dsum / 1000.0 / s_clk_dn : 0.0, s_clk_dmax / 1000.0,
             (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             ic_ram, ic_sd, em, (unsigned)(em_b / 1024), s_bg_on_sd, s_pet_frames);
    s_pet_frames = 0;
    s_lock_max = 0;
    s_clk_dsum = s_clk_dmax = 0;
    s_clk_dn = 0;
    s_ts_reads = s_ts_presses = s_ts_frames = 0;
    s_ts_time = s_ts_gap_max = s_ts_fsum = s_ts_fmax = 0;
    cJSON *o = cJSON_Parse(buf);
    if (o) {
        link_send(o);
        cJSON_Delete(o);
    }
}

static void bench_ev(lv_event_t *e)
{
    if (!s_bn_on) return;
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_bn_t0 = esp_timer_get_time();
    } else if (s_bn_t0) {
        int64_t d = esp_timer_get_time() - s_bn_t0;
        s_bn_t0 = 0;
        s_bn_sum += d;
        if (d > s_bn_max) s_bn_max = d;
        s_bn_frames++;
    }
}

static bool s_bn_y;   // a tela rola na vertical (retrato, listas)

static void bench_scroll(void *obj, int32_t v)
{
    if (s_bn_y) lv_obj_scroll_to_y(obj, v, LV_ANIM_OFF);
    else lv_obj_scroll_to_x(obj, v, LV_ANIM_OFF);
}

static void bench_done(lv_timer_t *t)
{
    s_bn_on = false;
    int64_t total = esp_timer_get_time() - s_bn_start;
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"t\":\"bench\",\"frames\":%d,\"avg_ms\":%.1f,\"max_ms\":%.1f,\"fps\":%.1f,\"heap\":%u}",
             s_bn_frames, s_bn_frames ? s_bn_sum / 1000.0 / s_bn_frames : 0.0, s_bn_max / 1000.0,
             s_bn_frames * 1e6 / (double)total, (unsigned)esp_get_free_heap_size());
    cJSON *o = cJSON_Parse(buf);
    if (o) {
        link_send(o);
        cJSON_Delete(o);
    }
    lv_timer_delete(t);
}

static void bench_start(int maxw)
{
    static bool hooked;
    lv_display_t *d = lv_display_get_default();
    if (!hooked) {
        lv_display_add_event_cb(d, bench_ev, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(d, bench_ev, LV_EVENT_REFR_READY, NULL);
        hooked = true;
    }
    lv_obj_update_layout(s_content);
    int32_t w = lv_obj_get_scroll_right(s_content) + lv_obj_get_scroll_x(s_content);
    s_bn_y = w <= 0;
    if (s_bn_y) w = lv_obj_get_scroll_bottom(s_content) + lv_obj_get_scroll_y(s_content);
    if (w > maxw) w = maxw;
    s_bn_frames = 0;
    s_bn_sum = s_bn_max = s_bn_t0 = 0;
    s_bn_start = esp_timer_get_time();
    s_bn_on = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_content);
    lv_anim_set_exec_cb(&a, bench_scroll);
    lv_anim_set_values(&a, 0, w > 0 ? w : 0);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_playback_duration(&a, 1000);
    lv_anim_start(&a);
    lv_timer_create(bench_done, 2100, NULL);
}

void ui_handle(const cJSON *msg)
{
    const char *t = jstr(msg, "t", "");

    // Mensagens que não mexem no LVGL
    if (!strcmp(t, "ping")) {
        link_send_event("pong", NULL, NULL, NULL, NULL);
        return;
    }
    if (!strcmp(t, "icon") && jbool(msg, "probe", false)) {
        const char *id = jstr(msg, "id", "");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "iconack");
        cJSON_AddStringToObject(o, "id", id);
        cJSON_AddBoolToObject(o, "ok", icon_probe(id));
        link_send(o);
        cJSON_Delete(o);
        return;
    }
    if (!strcmp(t, "stats")) {
        lvgl_port_lock(0);
        ts_hook();
        ts_report();
        lvgl_port_unlock();
        return;
    }
    if (!strcmp(t, "shot")) {
        lvgl_port_lock(0);
        shot_take();
        lvgl_port_unlock();
        return;
    }
    if (!strcmp(t, "bench")) {
        lvgl_port_lock(0);
        bench_start(jint(msg, "px", 330));
        lvgl_port_unlock();
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
    if (!strcmp(t, "jpg")) {
        handle_jpg(msg);
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
    int64_t lock_t0 = esp_timer_get_time();
    if (!strcmp(t, "screen")) {
        upd_drop();   // tela nova: a atualização guardada era da anterior
        render_screen(msg);
    } else if (!strcmp(t, "update")) {
        if (upd_hold()) {   // dedo no ecrã ou a rolar: guarda só a mais recente
            upd_drop();
            s_upd_pending = cJSON_Duplicate(msg, true);
        } else {
            upd_drop();
            apply_update(msg);
        }
    } else if (!strcmp(t, "about")) {   // {"host":"versão"}; "show":n abre a cena n (testes, capturas)
        const char *hv = jstr(msg, "host", NULL);
        if (hv) strlcpy(s_host_ver, hv, sizeof(s_host_ver));
        const cJSON *sh = cJSON_GetObjectItemCaseSensitive(msg, "show");
        if (cJSON_IsNumber(sh)) {
            s_about_var = sh->valueint % ABOUTANIM_VARIANTS;
            about_show();
        }
        const cJSON *tp = cJSON_GetObjectItemCaseSensitive(msg, "tap");   // [x, y]: simula um toque
        if (cJSON_GetArraySize(tp) == 2)
            about_tap(cJSON_GetArrayItem(tp, 0)->valueint, cJSON_GetArrayItem(tp, 1)->valueint);
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
        set_text(s_status, jstr(msg, "text", ""));
    } else if (!strcmp(t, "hdr")) {
        hdr_set(jint(msg, "plane", 0));
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
    int64_t held = esp_timer_get_time() - lock_t0;
    if (held > s_lock_max) {
        s_lock_max = held;
        strlcpy(s_lock_type, t, sizeof(s_lock_type));
    }
    lvgl_port_unlock();
}
