#pragma once
// Transporte DashDeck: linhas JSON pela USB-Serial-JTAG.
// Linhas iniciadas por '#' são logs do ESP (o host só as regista).
#include <stdbool.h>
#include "cJSON.h"

typedef void (*link_msg_cb_t)(const cJSON *msg);

void link_init(link_msg_cb_t on_msg);

// Envia um objeto JSON como uma linha. Não bloqueia por muito tempo se o host estiver ausente.
void link_send(const cJSON *msg);

// Envia uma linha já formatada (sem o '\n'), com timeout longo: para rajadas como a captura do ecrã.
void link_send_line(const char *s, size_t len);

// Envia {"t":type, ...} com até dois pares string opcionais (key=NULL para omitir).
void link_send_event(const char *type, const char *k1, const char *v1, const char *k2, const char *v2);

// true se recebemos algo do host nos últimos segundos.
bool link_host_alive(void);
