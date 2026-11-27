#pragma once
// Animações do Deck³ no estilo do DashDeck (pixel art desenhada quadro a quadro: pixels que voam para
// formar o nome, ondas de rádio, texto escrito letra a letra, estrelas, barra de progresso), elevadas
// ao cubo: os pixels passam a voxels que voam e montam modelos 3D (voxel.c).
// Desenho em buffers ARGB8888/XRGB8888 (stride em píxeis). O S3 tem FPU: contas em float.
#include <stdbool.h>
#include <stdint.h>

#include "voxel.h"

// ---- fonte pixel 5x7 do DashDeck (maiúsculas, dígitos, pontuação; acentos viram a letra base; ³)
int pfont_width(const char *s, int sc);
// Desenha os primeiros `nchars` carateres (-1 = todos). outline: 0 = sem contorno, senão cor RGB888.
void pfont_draw(uint32_t *buf, int w, int h, int stride, const char *s, int x, int y, int sc, uint32_t col,
                uint32_t outline, int nchars);

// ---- arranque (ecrã inteiro): estrelas, o cubo monta-se voxel a voxel, ondas, o nome forma-se com
// pixels que voam, a frase, e a barra de progresso com o estado da ligação.
#define BOOT_SEQ 3.6f   // segundos da sequência; depois fica em ciclo (cubo a rodar, ondas)
void anim_boot_start(int w, int h);
void anim_boot_draw(uint32_t *buf, int stride, float t, float progress, const char *status, float tilt);

// ---- aviso: ondas a fechar sobre o centro, os voxels montam o modelo, que pulsa; o nome escreve-se
// letra a letra por baixo. kind: 0 mensagem (verde LoRa), 1 alarme (dourado), 2 emergência (vermelho).
#define NOTIFY_ANIM 1.7f   // a faixa com o texto sobe a partir daqui
void anim_notify_start(const vox_model_t *m, const char *name, int kind, bool tint);   // tint: cor do tipo
void anim_notify_draw(uint32_t *buf, int w, int h, int stride, float t);
uint32_t anim_notify_color(int kind, int which);   // 0 principal, 1 claro, 2 escuro, 3 tinta

// ---- "Sobre o Deck³" (toque no título da tela inicial): salto no hiperespaço até ao cubo.
void anim_about_start(int w, int h);
void anim_about_draw(uint32_t *buf, int stride, float t, float tilt);

// ---- "fora de casa" (sem o uConsole): relógio grande, data, bateria, eventos guardados no cartão.
// Desenho leve (o chamador refresca 4 vezes por segundo); a posição desloca-se a cada minuto (AMOLED).
typedef struct {
    const char *time;     // "12:34" ou "--:--"
    const char *date;     // "QUI 26 NOV"
    int bat;              // % (-1 sem bateria)
    bool chg;
    int pending;          // eventos guardados no cartão
    bool sd;
    const char *status;   // estado da ligação
    int shift;            // deslocamento anti-marcas (0..8)
} away_t;
void anim_away_draw(uint32_t *buf, int w, int h, int stride, float t, const away_t *a);

// ---- relógio do uConsole (tpl "clock"): faces em ecrã inteiro (0 aurora boreal, 1 o cubo).
#define ANIM_CLOCK_FACES 2
typedef struct {
    const char *time;     // "12:34"
    const char *date;     // "QUI 26 NOV"
    int bat;              // % do cubo (-1 sem bateria)
    bool chg;
    int shift;            // deslocamento anti-marcas (0..8)
    bool has_wx;          // previsão do uConsole
    int temp, tmax, tmin, rain;
    bool has_icon;
    uint8_t icon[256];    // ícone do tempo 16x16 (índices na paleta, 0 = transparente)
    uint32_t icon_pal[8];
} clock3_t;
const char *anim_clock_name(int face);
void anim_clock_draw(int face, uint32_t *buf, int w, int h, int stride, float t, const clock3_t *c);
