#pragma once
// Animação "Sobre o DashDeck": cenas em pixel art por baixo dos créditos (ver aboutanim.c).
#include <stdbool.h>
#include <stdint.h>

#define ABOUTANIM_VARIANTS 2

void aboutanim_start(int variant, int w, int h);
// Desenha o instante t (s) no buffer RGB565 (w x h do start).
void aboutanim_draw(uint16_t *buf, int stride_px, float t);
const char *aboutanim_name(int variant);
// Toque em (x, y) no instante t. Devolve true se a cena o usou (então o toque não fecha a animação).
bool aboutanim_touch(int x, int y, float t);
// Instante (s) em que os créditos aparecem nesta cena (no Hiperespaço, só depois do salto).
float aboutanim_credits_at(int variant);
