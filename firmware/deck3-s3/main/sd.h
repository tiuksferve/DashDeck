#pragma once
// Cartão SD (SDMMC 1 bit: CMD 1, CLK 2, D0 3), montado em /sdcard. Os ficheiros do DashDeck³ ficam em
// /sdcard/deck3/ (ícones das boxes em icons/<id>.bin, eventos de fora de casa, imagens).
// O host gere-os pela ligação: "sd?", "ls", "file" (escrever, base64, em pedaços), "rm".
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "link.h"

#define SD_ROOT "/sdcard/deck3"

void sd_init(void);
bool sd_ok(void);
// caminho absoluto de um ficheiro relativo a SD_ROOT; false se inválido (".." ou absoluto)
bool sd_path(const char *rel, char *out, size_t n);
bool sd_write_text(const char *rel, const char *text);   // grava (substitui) um ficheiro de texto
char *sd_read_text(const char *rel, size_t max);          // malloc (PSRAM); NULL se não houver
bool sd_handle(const cJSON *msg, link_via_t via);

// Escritas em segundo plano (tarefa própria com stack na RAM interna: as outras tarefas podem ter o
// stack na PSRAM). Copia os dados; false se a fila estiver cheia ou não houver cartão.
bool sd_append(const char *rel, const void *data, size_t len);

// ---- modo "fora de casa": eventos guardados enquanto não há ligação ao deckd
// Guarda o evento (com "ts", hora UTC) em deck3/log/pendentes.jsonl.
void sd_event_store(const cJSON *msg);
int sd_events_pending(void);
// Entrega os eventos guardados ao deckd ({"t":"offline_log","events":[...]}) e apaga o ficheiro.
void sd_events_flush(void);
