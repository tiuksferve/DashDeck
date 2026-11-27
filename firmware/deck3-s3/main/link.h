#pragma once
// Barramento de mensagens do DashDeck³: o mesmo protocolo do DashDeck (um objeto JSON por mensagem) por
// dois transportes:
//   - Wi-Fi: WebSocket para o deckd (net.c), o transporte normal;
//   - USB-Serial-JTAG: linhas JSON, de reserva e para configurar (Wi-Fi, token) sem rede.
// As mensagens recebidas passam todas por uma fila e são tratadas por uma só tarefa, por ordem.
// As respostas vão pelo transporte de onde veio a última mensagem do host.
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

typedef enum { LINK_NONE = 0, LINK_USB, LINK_WIFI } link_via_t;

typedef void (*link_msg_cb_t)(const cJSON *msg, link_via_t via);
typedef void (*link_hello_cb_t)(link_via_t via);

void link_init(link_msg_cb_t on_msg, link_hello_cb_t on_hello);

// Os transportes entregam aqui o texto recebido (é copiado).
void link_push(const char *data, size_t len, link_via_t via);
// O WebSocket ligou ou caiu.
void link_transport_up(link_via_t via);
void link_transport_down(link_via_t via);

void link_send(const cJSON *msg);                       // pelo transporte ativo
void link_send_text(const char *json);                  // texto JSON já feito, pelo transporte ativo (capturas)
void link_send_via(const cJSON *msg, link_via_t via);   // por um transporte específico
// {"t":type, k1:v1, k2:v2} (key NULL para omitir)
void link_send_event(const char *type, const char *k1, const char *v1, const char *k2, const char *v2);

link_via_t link_active(void);
bool link_host_alive(void);   // mensagem do host nos últimos segundos
const char *link_via_name(link_via_t via);
