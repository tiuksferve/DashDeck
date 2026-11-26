#pragma once
// Wi-Fi e ligação ao deckd.
//  - Até 8 redes guardadas na NVS; liga-se à conhecida com melhor sinal (casa, hotspot do uConsole, ...).
//  - Encontra o deckd por esta ordem: endereço fixo (se configurado) → mDNS "_deck3._tcp" →
//    último endereço que funcionou → gateway da rede (o uConsole em modo hotspot) na porta 8765.
//  - WebSocket ws://host:porta/deck?id=<mac>&token=<token>; o deckd recusa tokens errados.
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "link.h"

#define DECK3_WS_PORT 8765

typedef enum {
    NET_NO_CREDS,        // nenhuma rede guardada: configurar pela USB (setup/provision.py)
    NET_SCANNING,
    NET_CONNECTING,
    NET_DISCOVERING,     // com IP, à procura do deckd
    NET_HOST_CONNECTING,
    NET_ONLINE,          // WebSocket ligado
} net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];
    char ip[16];
    char host[64];       // ip:porta do deckd (ou o que está a tentar)
    int rssi;            // dBm, 0 se desligado
    int nets;            // redes guardadas
} net_info_t;

void net_init(void);
void net_get_info(net_info_t *out);
const char *net_state_text(net_state_t s);

bool net_ws_connected(void);
int net_ws_send(const char *data, size_t len);   // 0 = enviado

// Mensagens de configuração ("wifi", "wifi?", "host", "token", "cfg?"). true = tratada aqui.
// "token" e "host" só são aceites pela USB.
bool net_handle_cfg(const cJSON *msg, link_via_t via);

// Identificador do aparelho (MAC em hex), também no hello.
const char *net_device_id(void);
