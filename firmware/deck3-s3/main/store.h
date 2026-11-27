#pragma once
// Definições do aparelho guardadas na NVS (namespace "deck3"): brilho, volume, temporizador...
// Ficam no cubo para valerem também sem o uConsole por perto.
#include <stdint.h>

int32_t store_get(const char *key, int32_t def);
void store_set(const char *key, int32_t v);   // só escreve se mudou
