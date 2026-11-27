#pragma once
// Varanda do Romeu (cubo em pé girado 90° para a esquerda): a cena pintada (fundo de 480x480 com 256 cores),
// o Romeu em sprites e os eventos (aviões, barcos, DMs, tempo) por cima. As imagens vêm do dashd para o
// cartão (varanda/). O desenho e o comportamento correm na tarefa de quadros (ui.c); os eventos chegam por
// uma fila, de qualquer tarefa.
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#define VAR_W 480
#define VAR_H 480

enum { VEV_TOUCH, VEV_KNOCK, VEV_SHAKE, VEV_CLAP, VEV_NOISE, VEV_CHARGE, VEV_PET, VEV_DM, VEV_JSON };

bool varanda_load(void);                       // lê as imagens do cartão (uma vez); false se faltarem
bool varanda_loaded(void);
void varanda_unload(void);                     // liberta as imagens (o dashd enviou outras)
void varanda_reset(void);                      // ao abrir: o Romeu volta ao deck, eventos limpos
void varanda_event(int type, int a, int b, const char *s1, const char *s2);
void varanda_event_json(const cJSON *msg);     // {"t":"varanda","wx":...,"planes":[...],"boats":[...]}
// Desenha o instante t (s) em buf (RGB565, VAR_W x VAR_H, direito para quem olha). Chamar com o trinco do anim.c.
void varanda_draw(uint16_t *buf, float t);
