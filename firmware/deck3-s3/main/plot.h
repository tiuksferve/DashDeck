#pragma once
// Template "plot" do DashDeck (céu do GPS, mapas do ADS-B/AIS/Sondas/Meshtastic, gráficos): canvas
// RGB565 com anéis, cruz, pontos cardeais, pontos com rumo e ícones (avião, navio, balão), trajetos
// (base64url, desenhados direto no buffer), textos soltos, etiqueta do alvo seguido, aviso ao centro,
// botões por cima ("ctrl") e arrasto do mapa ("drag": pan, toque, duplo toque). Protocolo: o do DashDeck
// (docs/protocolo.md). Canvas "live": imagens JPEG da câmara ({"t":"jpg"}).
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

// Canvas "live" (câmara): {"live":true} no plot. Os updates da mesma tela mantêm a imagem; plot_jpg desenha
// um JPEG (centrado; se for maior que o canvas, fica cortado). Chamar com o lock do LVGL.
bool plot_jpg(const char *screen_id, const uint8_t *jpg, size_t len);
void plot_detach(void);        // o corpo da tela vai ser apagado (o buffer "live" fica)
void plot_release(void);       // liberta o buffer "live" (o corpo da tela já foi apagado)
void plot_live_forget(void);   // tela nova: o próximo plot "live" começa limpo
