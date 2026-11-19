#pragma once
// Modo relógio: desenho pixel art direto num buffer RGB565 (sem LVGL).
// Mostrador 0 = paisagem (o fundo vem do host; aqui desenha-se a hora, a previsão e a chuva/neve),
// mostrador 1 = digital retro (todo desenhado aqui).
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum { CLK_ANIM_NONE, CLK_ANIM_RAIN, CLK_ANIM_STORM, CLK_ANIM_SNOW } clk_anim_t;

typedef struct {
    int face;               // 0 paisagem, 1 digital retro
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
} clock_state_t;

// Desenha o mostrador por cima do que já está no buffer (no 0, o fundo da paisagem).
void clockface_draw(uint16_t *buf, int stride_px, int w, int h, const clock_state_t *st, const struct tm *tm,
                    uint32_t ms);

// true se o mostrador tem animação (precisa de redesenho frequente).
bool clockface_animated(const clock_state_t *st);

// Fonte pixel 5x7 (a mesma do relógio), para outros desenhos (ex.: animação de arranque).
// Largura do texto em px na escala sc.
int clockfont_width(const char *s, int sc);
// Células acesas do texto (canto superior esquerdo de cada bloco sc x sc e o índice da letra).
// Devolve quantas escreveu (no máximo max).
int clockfont_cells(const char *s, int x, int y, int sc, int16_t (*out)[3], int max);
