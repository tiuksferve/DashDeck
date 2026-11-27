#pragma once
// Interface do DashDeck³: renderizador fino (como o DashDeck). O host manda telas genéricas em JSON e o
// ESP devolve toques. Templates compatíveis com o DashDeck (menu, list, kv, text, buttons), com
// ícones do dashd (ou placas de 8 bits), arranque animado, avisos e confirmações. Protocolo: docs/protocolo.md.
#include "cJSON.h"
#include "link.h"
#include "lvgl.h"

void ui_init(lv_display_t *disp);
void ui_handle(const cJSON *msg, link_via_t via);   // chamada pela tarefa do link (toma o lock)
void ui_send_hello(link_via_t via);

// Seguras a partir de qualquer tarefa (tomam o lock do LVGL)
void ui_toast(const char *text, const char *level);   // level: ok, warn, err, info
void ui_progress(const char *text, int pct);          // pct < 0 esconde
void ui_wake(void);                                   // acende o ecrã (toque, botão, IMU)
void ui_local_back(void);                             // BOOT sem host: fecha avisos/diálogos
void ui_local_bl_cycle(void);
bool ui_bl_off(void);                                 // ecrã desligado no botão de brilho (0%)                         // PWR sem host: alterna o brilho
void ui_orient(int ori);                              // IMU, cubo em pé: 0 normal, 1 girado à direita (Pomodoro), 2 à esquerda (varanda)
void ui_imu_event(int ev);
bool ui_rotated_open(void);                           // Pomodoro ou varanda abertos pelo giro (não pelos testes)                            // IMU: 1 batida na mesa, 2 agitar
bool ui_pomo_key(int key, int ev);                    // botão com o Pomodoro aberto (true = usado)
