#pragma once
// Template "plot" do DashDeck (céu do GPS, mapas do ADS-B/AIS/Sondas/Meshtastic, gráficos): canvas
// RGB565 com anéis, cruz, pontos cardeais, pontos com rumo e ícones (avião, navio, balão), trajetos
// (base64url, desenhados direto no buffer), textos soltos, etiqueta do alvo seguido, aviso ao centro,
// botões por cima ("ctrl") e arrasto do mapa ("drag": pan, toque, duplo toque). Protocolo: o do DashDeck
// (docs/protocolo.md).
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "lvgl.h"

typedef struct {
    void *(*alloc)(size_t bytes);                      // memória que vive com a tela
    uint32_t (*color)(const char *name, uint32_t def); // "#rrggbb", ok, warn, err, sub, accent
    const char *(*screen_id)(void);
    void (*send_btn)(const char *btn);                 // {"t":"btn","id":<tela>,"btn":..}
    void (*send_swipe)(const char *dir);
    void (*drag)(bool on);                             // arrasto em curso: adia os redesenhos da tela
} plot_env_t;

// Cria o plot (canvas + botões) dentro de parent e devolve o contentor.
lv_obj_t *plot_create(lv_obj_t *parent, const cJSON *plot, const plot_env_t *env);
