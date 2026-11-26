// Template "plot", portado do DashDeck (firmware/main/ui.c: draw_plot, draw_tracks, draw_icons, draw_tag,
// build_ctrl, map_ev) para o ecrã de 480x480: texto 16 px, ícones dos alvos 1,5x, botões maiores.
#include "plot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_fonts.h"
#include "esp_timer.h"

#define C_PLOT_BG 0x05070A
#define C_BAR     0x111821
#define C_GRID    0x26313E
#define C_TEXT    0xE8F0FF
#define C_SUB     0x8A97A8
#define C_ACCENT  0x3EC6FF
#define C_WARN    0xFFB23E
#define C_ERR     0xFF5470
#define PLOT_PTS  80
#define CTRL_MAX  10
#define MAP_EDGE  30
#define FONT      (&deck_font_16)

static const plot_env_t *E;

// buffer RGB565 do canvas atual (para os desenhos diretos)
static uint16_t *s_px;
static int32_t s_w, s_h, s_stride;   // stride em píxeis
static lv_obj_t *s_wrap, *s_cv;
static char s_ctrl_ids[CTRL_MAX][24];

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int32_t jint(const cJSON *o, const char *k, int32_t def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *k, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static lv_color_t col(const char *s, uint32_t def)
{
    return lv_color_hex(E->color(s, def));
}

static inline uint16_t rgb565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

// ---------------------------------------------------------------------------------------- desenho

static void text(lv_layer_t *layer, const char *txt, int32_t x, int32_t y, int32_t w, lv_color_t c, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;
    d.font = FONT;
    d.color = c;
    d.align = al;
    lv_area_t a = {.y1 = y - 9, .y2 = y + 10};
    if (al == LV_TEXT_ALIGN_CENTER) { a.x1 = x - w / 2; a.x2 = x + w / 2; }
    else if (al == LV_TEXT_ALIGN_RIGHT) { a.x1 = x - w; a.x2 = x; }
    else { a.x1 = x; a.x2 = x + w; }
    lv_draw_label(layer, &d, &a);
}

static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int32_t w)
{
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.p1.x = x1; l.p1.y = y1; l.p2.x = x2; l.p2.y = y2;
    l.color = c;
    l.width = w;
    lv_draw_line(layer, &l);
}

static inline void px_put(int32_t x, int32_t y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < s_w && y < s_h) s_px[y * s_stride + x] = c;
}

static void raw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c, int wdt)
{
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= s_w && x1 >= s_w) || (y0 >= s_h && y1 >= s_h)) return;
    int32_t dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
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

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// trajetos: [cor, largura, pontos] (4 carateres base64url por ponto, px + 1024)
static void draw_tracks(const cJSON *tracks)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, tracks) {
        const cJSON *jc = cJSON_GetArrayItem(t, 0), *jw = cJSON_GetArrayItem(t, 1), *jd = cJSON_GetArrayItem(t, 2);
        if (!cJSON_IsString(jd)) continue;
        uint16_t c = rgb565(E->color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_SUB));
        int wdt = (cJSON_IsNumber(jw) ? jw->valueint : 1) + 1;   // 480 px: um pouco mais grossos
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

// ---- ícones dos alvos (avião, navio, balão), rodados pelo rumo, com contorno escuro
typedef struct { int8_t x0, y0, x1, y1, x2, y2; } tri8_t;
static const tri8_t ICON_PLANE[] = {
    {0, -16, -3, -9, 3, -9}, {-3, -9, 3, -9, 3, 10}, {-3, -9, 3, 10, -3, 10}, {0, -7, -17, 5, 17, 5}, {0, 5, -8, 14, 8, 14},
};
static const tri8_t ICON_SHIP[] = {{0, -18, -7, -6, 7, -6}, {-7, -6, 7, -6, 6, 15}, {-7, -6, 6, 15, -6, 15}};
static const tri8_t ICON_BALLOON_NECK[] = {{-4, -12, 4, -12, 0, -6}};
#define ICON_K 12   // escala: vértices em 1/2 px x 1,5 (1/16 px: 8 * 1,5)

static void fill_tri16(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t c)
{
    int32_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (!area) return;
    int32_t mnx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t mxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t mny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t mxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int32_t ax = mnx >> 4, bx = (mxx >> 4) + 1, ay = mny >> 4, by = (mxy >> 4) + 1;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (bx >= s_w) bx = s_w - 1;
    if (by >= s_h) by = s_h - 1;
    for (int32_t py = ay; py <= by; py++) {
        int32_t sy = py * 16 + 8;
        for (int32_t px = ax; px <= bx; px++) {
            int32_t sx = px * 16 + 8;
            int32_t w0 = (x1 - x0) * (sy - y0) - (y1 - y0) * (sx - x0);
            int32_t w1 = (x2 - x1) * (sy - y1) - (y2 - y1) * (sx - x1);
            int32_t w2 = (x0 - x2) * (sy - y2) - (y0 - y2) * (sx - x2);
            if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) s_px[py * s_stride + px] = c;
        }
    }
}

static void fill_circle(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    for (int32_t y = -r; y <= r; y++)
        for (int32_t x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) px_put(cx + x, cy + y, c);
}

static void draw_tris(const tri8_t *t, int n, int32_t px, int32_t py, float sn, float cs, uint16_t c)
{
    for (int i = 0; i < n; i++) {
        int32_t v[6] = {t[i].x0, t[i].y0, t[i].x1, t[i].y1, t[i].x2, t[i].y2}, o[6];
        for (int k = 0; k < 6; k += 2) {
            o[k] = px * 16 + 8 + (int32_t)((v[k] * cs - v[k + 1] * sn) * ICON_K);
            o[k + 1] = py * 16 + 8 + (int32_t)((v[k] * sn + v[k + 1] * cs) * ICON_K);
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
            uint16_t cc = pass ? c : dark;
            int e = pass ? 0 : 1;
            fill_circle(px, py - 16, 8 + e, cc);
            draw_tris(ICON_BALLOON_NECK, 1, px, py, 0, 1, cc);
            for (int32_t y = py - 7 - e; y <= py - 3 + e; y++)
                for (int32_t x = px - e; x <= px + e; x++) px_put(x, y, cc);
            for (int32_t y = py - 2 - e; y <= py + 2 + e; y++)
                for (int32_t x = px - 2 - e; x <= px + 2 + e; x++) px_put(x, y, cc);
        }
        return;
    }
    const tri8_t *t = !strcmp(shape, "ship") ? ICON_SHIP : ICON_PLANE;
    int n = t == ICON_SHIP ? 3 : 5;
    float sn = 0, cs = 1;
    if (cJSON_IsNumber(jh)) {
        float rad = (float)jh->valuedouble * (float)M_PI / 180.0f;
        sn = sinf(rad);
        cs = cosf(rad);
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
        draw_icon(js->valuestring, (int32_t)jx->valuedouble, (int32_t)jy->valuedouble, cJSON_GetArrayItem(pt, 4),
                  rgb565(E->color(cJSON_IsString(jc) ? jc->valuestring : NULL, C_TEXT)));
    }
}

// etiqueta do alvo seguido: [x, y, cor, "linha1\nlinha2.."], numa caixa ao lado do ponto
static void draw_tag(lv_layer_t *layer, const cJSON *tag, int32_t w, int32_t h)
{
    const cJSON *jx = cJSON_GetArrayItem(tag, 0), *jy = cJSON_GetArrayItem(tag, 1);
    const cJSON *jc = cJSON_GetArrayItem(tag, 2), *jt = cJSON_GetArrayItem(tag, 3);
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) || !cJSON_IsString(jt)) return;
    int32_t x = (int32_t)jx->valuedouble, y = (int32_t)jy->valuedouble;
    lv_color_t c = col(cJSON_IsString(jc) ? jc->valuestring : NULL, C_TEXT);
    char buf[200];
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
        lv_point_t sz;
        lv_text_get_size(&sz, ln[i], FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t lw = sz.x;
        if (lw > tw) tw = lw;
    }
    int32_t bw = tw + 14, bh = n * 20 + 8;
    if (bw > w - 4) bw = w - 4;
    const int32_t cand[6][2] = {{x + 14, y - 14 - bh}, {x - 14 - bw, y - 14 - bh}, {x + 14, y - bh / 2},
                                {x - 14 - bw, y - bh / 2}, {x + 14, y + 14}, {x - 14 - bw, y + 14}};
    int32_t bx = cand[0][0], by = cand[0][1];
    bool ok = false;
    for (int k = 0; k < 6 && !ok; k++) {
        if (cand[k][0] < 2 || cand[k][1] < 2 || cand[k][0] + bw > w - 2 || cand[k][1] + bh > h - 2) continue;
        bx = cand[k][0];
        by = cand[k][1];
        ok = true;
    }
    if (!ok) {
        if (bx + bw > w - 2) bx = x - 14 - bw;
        if (bx < 2) bx = 2;
        if (by < 2) by = y + 14;
        if (by + bh > h - 2) by = h - 2 - bh;
    }
    line(layer, x, y, bx > x ? bx : bx + bw, by > y ? by : by + bh, c, 1);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(C_BAR);
    rd.bg_opa = LV_OPA_80;
    rd.radius = 6;
    rd.border_color = c;
    rd.border_width = 1;
    lv_area_t ar = {bx, by, bx + bw - 1, by + bh - 1};
    lv_draw_rect(layer, &rd, &ar);
    for (int i = 0; i < n; i++)
        text(layer, ln[i], bx + 7, by + 4 + 10 + i * 20, bw - 10, i ? lv_color_hex(C_TEXT) : c, LV_TEXT_ALIGN_LEFT);
}

static void draw(const cJSON *p, int32_t w, int32_t h)
{
    const int32_t cx = jint(p, "cx", w / 2), cy = jint(p, "cy", h / 2);
    const int32_t R = jint(p, "R", (w < h ? w : h) / 2 - 3);
    const lv_color_t grid = lv_color_hex(C_GRID), sub = lv_color_hex(C_SUB);
    uint16_t bg = rgb565(C_PLOT_BG);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) s_px[y * s_stride + x] = bg;
    draw_tracks(cJSON_GetObjectItemCaseSensitive(p, "tracks"));
    draw_icons(cJSON_GetObjectItemCaseSensitive(p, "pts"));
    lv_layer_t layer;
    lv_canvas_init_layer(s_cv, &layer);

    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx;
    a.center.y = cy;
    a.start_angle = 0;
    a.end_angle = 360;
    a.width = 1;
    a.color = grid;
    const cJSON *rl = cJSON_GetObjectItemCaseSensitive(p, "rlabels"), *rv;
    int ri = 0;
    cJSON_ArrayForEach(rv, cJSON_GetObjectItemCaseSensitive(p, "rings")) {
        if (cJSON_IsNumber(rv) && rv->valuedouble > 0) {
            int32_t r = (int32_t)rv->valuedouble;
            a.radius = r;
            lv_draw_arc(&layer, &a);
            const cJSON *lab = cJSON_GetArrayItem(rl, ri);
            if (cJSON_IsString(lab) && lab->valuestring[0]) {
                int32_t d = (int32_t)(r * 0.7071f);
                text(&layer, lab->valuestring, cx + d + 2, cy + d - 8, 70, sub, LV_TEXT_ALIGN_LEFT);
            }
        }
        ri++;
    }
    if (jbool(p, "cross", true)) {
        line(&layer, cx, cy - R, cx, cy + R, grid, 1);
        line(&layer, cx - R, cy, cx + R, cy, grid, 1);
    }
    if (jbool(p, "cardinal", false)) {
        text(&layer, "N", cx + 4, cy - R + 11, 16, lv_color_hex(C_ERR), LV_TEXT_ALIGN_LEFT);
        text(&layer, "S", cx + 4, cy + R - 11, 16, sub, LV_TEXT_ALIGN_LEFT);
        text(&layer, "E", cx + R - 2, cy - 11, 16, sub, LV_TEXT_ALIGN_RIGHT);
        text(&layer, "W", cx - R + 2, cy - 11, 22, sub, LV_TEXT_ALIGN_LEFT);
    }
    if (jbool(p, "center", false)) {
        lv_color_t ac = lv_color_hex(C_ACCENT);
        line(&layer, cx - 6, cy, cx + 6, cy, ac, 2);
        line(&layer, cx, cy - 6, cx, cy + 6, ac, 2);
    }
    // pontos: [x, y, cor, rótulo, rumo, cheio, ícone?]
    int n = 0;
    const cJSON *pt;
    cJSON_ArrayForEach(pt, cJSON_GetObjectItemCaseSensitive(p, "pts")) {
        if (n++ >= PLOT_PTS) break;
        const cJSON *jx = cJSON_GetArrayItem(pt, 0), *jy = cJSON_GetArrayItem(pt, 1);
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) continue;
        const cJSON *jc = cJSON_GetArrayItem(pt, 2), *jl = cJSON_GetArrayItem(pt, 3);
        const cJSON *jh = cJSON_GetArrayItem(pt, 4), *jf = cJSON_GetArrayItem(pt, 5);
        int32_t px = (int32_t)jx->valuedouble, py = (int32_t)jy->valuedouble;
        lv_color_t c = col(cJSON_IsString(jc) ? jc->valuestring : NULL, C_TEXT);
        bool icon = cJSON_IsString(cJSON_GetArrayItem(pt, 6));
        bool left = px > w - 70;
        if (!icon) {
            if (cJSON_IsNumber(jh)) {
                float rad = (float)jh->valuedouble * (float)M_PI / 180.0f;
                line(&layer, px, py, px + (int32_t)lroundf(14 * sinf(rad)), py - (int32_t)lroundf(14 * cosf(rad)), c, 2);
            }
            lv_draw_rect_dsc_t rd;
            lv_draw_rect_dsc_init(&rd);
            rd.radius = LV_RADIUS_CIRCLE;
            bool fill = !cJSON_IsNumber(jf) || jf->valueint;
            rd.bg_color = c;
            rd.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
            rd.border_color = c;
            rd.border_width = fill ? 0 : 2;
            lv_area_t ar = {px - 5, py - 5, px + 5, py + 5};
            lv_draw_rect(&layer, &rd, &ar);
        }
        if (cJSON_IsString(jl) && jl->valuestring[0]) {
            int off = icon ? 14 : 8;
            text(&layer, jl->valuestring, left ? px - off : px + off, py - 10, 80, c,
                 left ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
        }
    }
    // textos soltos: [x, y, cor, texto, "l|c|r"]
    const cJSON *tx;
    cJSON_ArrayForEach(tx, cJSON_GetObjectItemCaseSensitive(p, "texts")) {
        const cJSON *jx = cJSON_GetArrayItem(tx, 0), *jy = cJSON_GetArrayItem(tx, 1), *jt = cJSON_GetArrayItem(tx, 3);
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) || !cJSON_IsString(jt)) continue;
        const cJSON *jc = cJSON_GetArrayItem(tx, 2), *ja = cJSON_GetArrayItem(tx, 4);
        const char *al = cJSON_IsString(ja) ? ja->valuestring : "l";
        text(&layer, jt->valuestring, (int32_t)jx->valuedouble, (int32_t)jy->valuedouble, 110,
             col(cJSON_IsString(jc) ? jc->valuestring : NULL, C_SUB),
             al[0] == 'c' ? LV_TEXT_ALIGN_CENTER : al[0] == 'r' ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
    }
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(p, "tag");
    if (cJSON_IsArray(tag)) draw_tag(&layer, tag, w, h);
    const char *msg = jstr(p, "msg");
    if (msg && msg[0]) {
        lv_draw_rect_dsc_t bgd;
        lv_draw_rect_dsc_init(&bgd);
        bgd.bg_color = lv_color_hex(C_BAR);
        bgd.bg_opa = LV_OPA_80;
        bgd.radius = 8;
        lv_area_t ba = {10, cy - 26, w - 11, cy + 26};
        lv_draw_rect(&layer, &bgd, &ba);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = msg;
        d.text_local = 1;
        d.font = FONT;
        d.color = lv_color_hex(C_WARN);
        d.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t ta = {16, cy - 22, w - 17, cy + 24};
        lv_draw_label(&layer, &d, &ta);
    }
    lv_canvas_finish_layer(s_cv, &layer);
}

// ---------------------------------------------------------------------------------------- botões

static void ctrl_click_cb(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < CTRL_MAX && s_ctrl_ids[i][0]) E->send_btn(s_ctrl_ids[i]);
}

static void build_ctrl(const cJSON *ctrl)
{
    memset(s_ctrl_ids, 0, sizeof(s_ctrl_ids));
    int i = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, ctrl) {
        if (i >= CTRL_MAX) break;
        const cJSON *jid = cJSON_GetArrayItem(c, 0), *jpos = cJSON_GetArrayItem(c, 1), *jl = cJSON_GetArrayItem(c, 2);
        const cJSON *jc = cJSON_GetArrayItem(c, 3);
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
        strlcpy(s_ctrl_ids[i], jid->valuestring, sizeof(s_ctrl_ids[i]));
        lv_obj_t *b = lv_obj_create(s_wrap);
        lv_obj_remove_style_all(b);
        bool big = lbl && strlen(lbl) <= 3;
        lv_obj_set_size(b, LV_SIZE_CONTENT, big ? 52 : 42);
        lv_obj_set_style_min_width(b, big ? 60 : 42, 0);
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_align(b, al, al == LV_ALIGN_TOP_LEFT || al == LV_ALIGN_BOTTOM_LEFT || al == LV_ALIGN_LEFT_MID ? 4 :
                            al == LV_ALIGN_TOP_MID || al == LV_ALIGN_BOTTOM_MID ? 0 : -4,
                     al == LV_ALIGN_TOP_LEFT || al == LV_ALIGN_TOP_RIGHT || al == LV_ALIGN_TOP_MID ? 4 :
                     al == LV_ALIGN_LEFT_MID || al == LV_ALIGN_RIGHT_MID ? 0 : -4);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, col(cJSON_IsString(jc) ? jc->valuestring : NULL, C_BAR), 0);
        lv_obj_set_style_bg_opa(b, cJSON_IsString(jc) ? LV_OPA_90 : LV_OPA_70, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_GRID), 0);
        lv_obj_set_ext_click_area(b, 8);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, ctrl_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, lbl ? lbl : sym);
        lv_obj_set_style_text_font(l, big ? &deck_font_28 : &deck_font_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_center(l);
        i++;
    }
}

// ---------------------------------------------------------------------------------------- arrasto

static bool s_drag_on, s_moving, s_edge;
static lv_point_t s_start, s_last_pt;
static int64_t s_last_tap;
static lv_timer_t *s_tap_tm;
static char s_tap_btn[32], s_tap_scr[48];

static void tap_fire(lv_timer_t *t)
{
    s_tap_tm = NULL;
    if (!strcmp(s_tap_scr, E->screen_id())) E->send_btn(s_tap_btn);
}

static bool near_ctrl(lv_point_t p)
{
    if (!s_wrap) return false;
    for (uint32_t i = 1; i < lv_obj_get_child_count(s_wrap); i++) {
        lv_area_t a;
        lv_obj_get_coords(lv_obj_get_child(s_wrap, i), &a);
        if (p.x >= a.x1 - 16 && p.x <= a.x2 + 16 && p.y >= a.y1 - 16 && p.y <= a.y2 + 16) return true;
    }
    return false;
}

static void map_ev(lv_event_t *e)
{
    lv_obj_t *cv = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || !s_drag_on) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        lv_area_t a;
        lv_obj_get_coords(cv, &a);
        s_start = p;
        s_moving = false;
        s_edge = p.x - a.x1 < MAP_EDGE || a.x2 - p.x < MAP_EDGE;
        E->drag(true);
        return;
    }
    int32_t dx = p.x - s_start.x, dy = p.y - s_start.y;
    if (code == LV_EVENT_PRESSING) {
        if (!s_moving && (LV_ABS(dx) > 8 || LV_ABS(dy) > 8)) s_moving = true;
        if (s_moving && !s_edge) {
            lv_obj_set_style_translate_x(cv, dx, 0);
            lv_obj_set_style_translate_y(cv, dy, 0);
        }
        return;
    }
    E->drag(false);
    if (code != LV_EVENT_RELEASED) {
        lv_obj_set_style_translate_x(cv, 0, 0);
        lv_obj_set_style_translate_y(cv, 0, 0);
        return;
    }
    if (s_moving) {
        s_last_tap = 0;
        if (s_edge) {
            if (LV_ABS(dx) > 50 && LV_ABS(dx) > LV_ABS(dy)) E->send_swipe(dx < 0 ? "left" : "right");
        } else {
            char b[32];
            snprintf(b, sizeof(b), "pan:%d,%d", (int)dx, (int)dy);
            E->send_btn(b);   // o deslocamento fica até chegar o mapa novo
        }
        return;
    }
    if (near_ctrl(p)) {
        s_last_tap = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_tap && now - s_last_tap < 400000 && LV_ABS(p.x - s_last_pt.x) < 40 && LV_ABS(p.y - s_last_pt.y) < 40) {
        s_last_tap = 0;
        if (s_tap_tm) {
            lv_timer_delete(s_tap_tm);
            s_tap_tm = NULL;
        }
        E->send_btn("mapinfo");
    } else {
        s_last_tap = now;
        s_last_pt = p;
        lv_area_t a;
        lv_obj_get_coords(cv, &a);
        snprintf(s_tap_btn, sizeof(s_tap_btn), "tap:%d,%d", (int)(p.x - a.x1), (int)(p.y - a.y1));
        strlcpy(s_tap_scr, E->screen_id(), sizeof(s_tap_scr));
        if (s_tap_tm) lv_timer_delete(s_tap_tm);
        s_tap_tm = lv_timer_create(tap_fire, 400, NULL);
        lv_timer_set_repeat_count(s_tap_tm, 1);
    }
}

// ---------------------------------------------------------------------------------------- criação

lv_obj_t *plot_create(lv_obj_t *parent, const cJSON *p, const plot_env_t *env)
{
    E = env;
    int32_t w = jint(p, "w", 160), h = jint(p, "h", 160);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w > 480) w = 480;
    if (h > 480) h = 480;
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    uint8_t *mem = E->alloc(stride * h);
    s_wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(s_wrap);
    lv_obj_set_size(s_wrap, w, h);
    lv_obj_remove_flag(s_wrap, LV_OBJ_FLAG_SCROLLABLE);
    if (!mem) return s_wrap;
    s_cv = lv_canvas_create(s_wrap);
    lv_canvas_set_buffer(s_cv, mem, w, h, LV_COLOR_FORMAT_RGB565);
    s_px = (uint16_t *)mem;
    s_w = w;
    s_h = h;
    s_stride = stride / 2;
    draw(p, w, h);
    s_drag_on = jbool(p, "drag", false);
    if (s_drag_on) {
        lv_obj_add_flag(s_cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_cv, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(s_cv, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(s_cv, map_ev, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_cv, map_ev, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_cv, map_ev, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_cv, map_ev, LV_EVENT_PRESS_LOST, NULL);
    }
    build_ctrl(cJSON_GetObjectItemCaseSensitive(p, "ctrl"));
    return s_wrap;
}
