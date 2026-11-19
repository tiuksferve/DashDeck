#pragma once
// Animação de arranque do DashDeck: 0 = "Pixels", 1 = "Nascer do sol".
#include <stdbool.h>
#include <stdint.h>

#define BOOTANIM_SECONDS 5.0f   // tempo mínimo da tela de carregamento (a sequência estica até aqui)

void bootanim_start(int variant, int w, int h);
// Desenha o instante t (s) no buffer RGB565 (w x h do start). Depois de BOOTANIM_SECONDS fica no
// último quadro, com o ambiente animado. progress 0..1 = barra em baixo; waiting = "A AGUARDAR O UCONSOLE".
void bootanim_draw(uint16_t *buf, int stride_px, float t, float progress, bool waiting);
