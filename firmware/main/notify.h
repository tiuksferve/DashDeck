#pragma once
// Aviso de mensagem direta ("Ondas LoRa"), desenhado num canvas pequeno ao centro do ecrã.
#include <stdint.h>

#define NOTIFY_W 160   // 46 KB em RGB565A8: cabe ao lado do relógio aberto
#define NOTIFY_H 96
#define NOTIFY_ANIM 1.9f      // s até a faixa com o texto aparecer
#define NOTIFY_TOTAL 5.5f     // s até o aviso desaparecer sozinho

void notify_start(const char *name);
// buf: NOTIFY_W x NOTIFY_H em RGB565A8 (cor e depois alfa), apagado e redesenhado a cada chamada.
void notify_draw(uint8_t *buf, float t);
