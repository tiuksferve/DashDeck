#pragma once
// Motor de voxels do Deck³: modelos pequenos (ícones até 12³, cenas até 24³) desenhados em 3D (projeção ortográfica,
// rotação livre, faces sombreadas com bisel) num buffer ARGB8888. Serve os ícones das boxes, a
// animação de arranque e os avisos. O ESP32-S3 tem FPU: as contas são em float.
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#define VOX_MAX 24   // ícones até 12; cenas do cartão até 24x24x24

typedef struct {
    uint8_t nx, ny, nz;
    uint8_t cells[VOX_MAX * VOX_MAX * VOX_MAX];   // índice na paleta (1..15), 0 = vazio
    uint32_t pal[16];                             // 0xRRGGBB
} vox_model_t;

typedef struct {
    float yaw;        // rotação em torno do eixo vertical (rad); 0.785 = vista isométrica
    float pitch;      // inclinação da câmara (rad); 0.6 ≈ 35°
    float scale;      // px por voxel; 0 = ajustar ao buffer
    float cx, cy;     // centro do modelo no buffer (px); 0,0 = centro do buffer
    uint32_t tint;    // 0 = cores do modelo; senão substitui a cor 1 da paleta (0xRRGGBB)
    float explode;    // 0 = montado; 1 = voxels espalhados (animação de montagem)
    float fly;        // > 0: montagem "à DashDeck": cada voxel voa de um ponto ao acaso para o seu lugar,
                      // camada a camada (de baixo para cima) com atrasos escalonados; fly = segundos desde
                      // o início + 1 (1 = começo). 0 = desligado. Duração total: vox_fly_time().
    float glow;       // 0..1: clareia o modelo (pulsar dos avisos)
} vox_view_t;

#define VOX_VIEW_DEFAULT { .yaw = 0.785f, .pitch = 0.6f, .scale = 0, .cx = 0, .cy = 0, .tint = 0, .explode = 0, .fly = 0, .glow = 0 }

// Segundos que a montagem "fly" demora para este modelo (do 1.º voxel a sair ao último a chegar).
float vox_fly_time(const vox_model_t *m);

// Desenha por cima do que já está no buffer (o chamador limpa-o). stride em píxeis.
void vox_render(const vox_model_t *m, const vox_view_t *v, uint32_t *buf, int w, int h, int stride);

// Modelo embutido, definido pelo host ou guardado no cartão (deck3/vox/<nome>.json); NULL se não existir.
const vox_model_t *vox_get(const char *name);

// {"t":"vox","id":"x","size":[nx,ny,nz],"pal":["#rrggbb",...],"layers":[["row",...],...]}
// layers de baixo para cima; cada linha tem nx caracteres: '.' vazio, '1'..'9','a'..'f' = paleta.
bool vox_define(const cJSON *msg);

// Nome de ícone do DashDeck (gps, wifi, settings...) -> nome de modelo de voxels.
const char *vox_for_icon(const char *icon);

// Esquece os nomes procurados sem sucesso no cartão (depois de lá chegar um ficheiro novo).
void vox_forget_missing(void);
