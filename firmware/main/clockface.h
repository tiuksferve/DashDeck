#pragma once
// Modo relógio: desenho pixel art direto num buffer RGB565 (sem LVGL).
// Mostrador 0 = paisagem (o fundo vem do host; aqui desenha-se a hora, a previsão e a chuva/neve),
// mostrador 1 = digital retro (todo desenhado aqui), mostrador 2 = previsão por horas (fundo da costa
// vindo do host, com a faixa das próximas horas desenhada aqui), mostrador 3 = cidade (como a paisagem,
// com outro fundo), mostrador 4 = temporizador e alarme (todo desenhado aqui, com zonas de toque).
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum { CLK_ANIM_NONE, CLK_ANIM_RAIN, CLK_ANIM_STORM, CLK_ANIM_SNOW } clk_anim_t;

#define CLK_FACES 5
#define CLK_FACE_TIMER 4
#define CLK_HAS_SCENE(f) ((f) == 0 || (f) == 2 || (f) == 3)   // fundo desenhado pelo host

// Zonas de toque do mostrador do temporizador e alarme
enum {
    CZ_NONE, CZ_TMIN_UP, CZ_TMIN_DN, CZ_TSEC_UP, CZ_TSEC_DN, CZ_TGO, CZ_TRESET,
    CZ_AH_UP, CZ_AH_DN, CZ_AM_UP, CZ_AM_DN, CZ_AON,
};

typedef struct {
    int face;               // 0 paisagem, 1 digital retro, 2 previsão, 3 cidade, 4 temporizador/alarme
    int pal;                // cor do LCD: 0 âmbar, 1 verde, 2 ciano
    bool has_time;
    bool has_wx;
    int temp, tmax, tmin, rain, wind;
    bool has_icon;
    uint8_t icon[256];      // 16x16, índice na paleta (0 = transparente)
    uint16_t icon_pal[8];   // RGB565
    uint8_t icon_role[8];   // 1 = forma, 2 = sombra (no LCD: cor acesa / meia cor)
    clk_anim_t anim;
    bool night;
    int bright;             // brilho atual (barra ao mudar)
    bool show_bright, show_dots;
    bool show_exit;         // 1.º toque: "toque de novo para sair"
    // previsão por horas (mostrador 2): até 6 horas e até 6 ícones diferentes
    int nh;
    struct { int8_t hour; int8_t icon; int8_t temp; uint8_t rain; } hours[6];   // hour < 0 = "agora"
    int nhicons;
    uint8_t hicon[6][256];
    uint16_t hpal[6][8];
    uint8_t hrole[6][8];
    // temporizador (s): a contar se tend > 0 (fim em epoch local); em pausa se tleft > 0; senão tset
    int tset, tleft;
    int64_t tend, now;      // now: epoch local atual (para o restante)
    int alh, alm;           // alarme diário
    bool alon;
    int hot;                // zona a ser tocada (realçada)
    // cão junto à casa da paisagem (mostrador 0): -1 nenhum, 0 a dormir, 1 a brincar com o dono.
    // pet_x/pet_y: canto esquerdo e base da casa; pet_l: luz do dia 0..100 (escurece as cores)
    int pet, pet_x, pet_y, pet_l;
} clock_state_t;

// Zona do mostrador do temporizador em (x, y), ou CZ_NONE.
int clockface_zone(const clock_state_t *st, int w, int h, int x, int y);

// Desenha o mostrador por cima do que já está no buffer (no 0, o fundo da paisagem).
void clockface_draw(uint16_t *buf, int stride_px, int w, int h, const clock_state_t *st, const struct tm *tm,
                    uint32_t ms);

// true se o mostrador tem animação (precisa de redesenho frequente).
bool clockface_animated(const clock_state_t *st);
// true se o cão está a brincar; a zona onde ele (e a bola) se mexem fica em x, y, w, h.
bool clockface_pet_rect(const clock_state_t *st, int *x, int *y, int *w, int *h);
// Só o cão (por cima do fundo já reposto nessa zona), para os quadros entre os desenhos completos.
void clockface_draw_pet(uint16_t *buf, int stride_px, int w, int h, const clock_state_t *st, uint32_t ms);

// Fonte pixel 5x7 (a mesma do relógio), para outros desenhos (ex.: animação de arranque).
// Largura do texto em px na escala sc.
int clockfont_width(const char *s, int sc);
// Células acesas do texto (canto superior esquerdo de cada bloco sc x sc e o índice da letra).
// Devolve quantas escreveu (no máximo max).
int clockfont_cells(const char *s, int x, int y, int sc, int16_t (*out)[3], int max);
