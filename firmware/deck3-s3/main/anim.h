#pragma once
// Animações do DashDeck³ no estilo do DashDeck: pixel art de jogo de 8 bits desenhada quadro a quadro
// (pixels que voam para formar o nome, ondas de rádio, texto escrito letra a letra, estrelas, meteoros,
// barra de progresso); as cenas grandes desenham-se a 240x240 e ampliam-se 2x.
// Desenho em buffers ARGB8888/XRGB8888 (stride em píxeis). O S3 tem FPU: contas em float.
#include <stdbool.h>
#include <stdint.h>


// ---- fonte pixel 5x7 do DashDeck (maiúsculas, dígitos, pontuação; acentos viram a letra base; ³)
int pfont_width(const char *s, int sc);
// Desenha os primeiros `nchars` carateres (-1 = todos). outline: 0 = sem contorno, senão cor RGB888.
void pfont_draw(uint32_t *buf, int w, int h, int stride, const char *s, int x, int y, int sc, uint32_t col,
                uint32_t outline, int nchars);

// ---- arranque (ecrã inteiro, pixel art de 8 bits ampliada 2x): as camadas do cubo caem, as letras de
// DASHDECK caem, o cubo salta para o expoente e transforma-se no ³; barra com o estado da ligação.
#define BOOT_SEQ 4.0f   // segundos da sequência; depois fica em ciclo (estrelas, brilho no nome)
void anim_boot_start(int w, int h);
void anim_boot_draw(uint32_t *buf, int stride, float t, float progress, const char *status, float tilt);

// ---- aviso: ondas a fechar sobre o centro, o sprite de 8 bits cai e assenta e depois mexe-se; o nome
// escreve-se letra a letra por baixo. kind: 0 mensagem (verde LoRa), 1 alarme (dourado), 2 emergência (vermelho).
#define NOTIFY_ANIM 1.7f   // a faixa com o texto sobe a partir daqui
enum { SPR_MAIL, SPR_CLOCK, SPR_PLANE };
int anim_sprite_id(const char *name);   // "mail" "clock" "plane" -> SPR_*; -1 se não existir
void anim_notify_start(int sprite, const char *name, int kind);
void anim_notify_draw(uint32_t *buf, int w, int h, int stride, float t);
uint32_t anim_notify_color(int kind, int which);   // 0 principal, 1 claro, 2 escuro, 3 tinta

// ---- ícone de reserva das boxes (sem imagem do dashd): placa de 8 bits com a inicial
void anim_badge(uint32_t *buf, int size, int stride, uint32_t col, const char *label);

// ---- "Sobre o DashDeck³" (toque no título da tela inicial): salto no hiperespaço até ao cubo.
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

// ---- relógio do uConsole (tpl "clock"): faces em ecrã inteiro.
#include "bg.h"
#define ANIM_CLOCK_FACES 4
#define CLK_FACE_TIMER 3
enum { CLK_NONE, CLK_RAIN, CLK_STORM, CLK_SNOW };   // chuva, trovoada, neve
// zonas de toque da face do temporizador e alarme
enum { CZ_NONE, CZ_TMIN_UP, CZ_TMIN_DN, CZ_TSEC_UP, CZ_TSEC_DN, CZ_TGO, CZ_TRESET,
       CZ_AH_UP, CZ_AH_DN, CZ_AM_UP, CZ_AM_DN, CZ_AON };
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
    const bgimg_t *scene; // paisagem (240x240, 8 bpp) enviada pelo dashd, ou NULL
    int anim;             // CLK_RAIN...
    bool night;
    int pet, pet_x, pet_y, pet_l;   // o cão junto à casa (-1 nenhum, 0 a dormir, 1 a brincar)
    int tset, tleft;      // temporizador: duração escolhida; em pausa, o que falta
    int64_t tend, now;    // a contar: fim (epoch local); agora (epoch local)
    int alh, alm;
    bool alon;
} clock3_t;
const char *anim_clock_name(int face);
void anim_clock_draw(int face, uint32_t *buf, int w, int h, int stride, float t, const clock3_t *c);
int anim_clock_zone(int face, int x, int y);   // zona de toque (CZ_*) na face do temporizador

// ---- Pomodoro (cubo em pé girado 90° para a direita). Desenhado direito para quem olha; o ui.c roda-o.
enum { POMO_FOCUS, POMO_SHORT, POMO_LONG };
enum { PZ_NONE, PZ_GO, PZ_RESET, PZ_SKIP };   // zonas de toque (nas coordenadas de quem olha)
typedef struct {
    int phase;            // POMO_*
    int total, left;      // duração da fase e o que falta (s)
    bool run;
    int done;             // focos concluídos neste ciclo (0..4; ao 4.º vem a pausa longa)
    int today;            // focos concluídos hoje
    const char *clock;    // "12:34" ou NULL
} pomo_t;
void anim_pomo_draw(uint32_t *buf, int w, int h, int stride, float t, const pomo_t *p);
int anim_pomo_zone(int x, int y);
