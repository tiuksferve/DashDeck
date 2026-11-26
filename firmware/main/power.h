#pragma once
// PMU AXP2101 (I2C 0x34): bateria, carga, USB e o botão PWR (interrupções de toque curto/longo).
// Só lê e ativa interrupções: não mexe nas tensões (que alimentam o ecrã e o ESP).
#include <stdbool.h>

typedef struct {
    bool ok;          // AXP2101 respondeu
    bool battery;     // bateria ligada
    bool charging;
    bool usb;         // VBUS presente
    int percent;      // 0..100, -1 sem bateria
    int mv;           // tensão da bateria
    int stage;        // REG 0x01[2:0]: 0 trickle, 1 pré-carga, 2 corrente constante, 3 tensão constante, 4 completa, 5 parada
    int vbus_mv;      // tensão da USB
    int vsys_mv;      // tensão do sistema
    float temp;       // temperatura do AXP2101 (°C)
    int icc_ma;       // corrente de carga configurada (só leitura)
    int cv_mv;        // tensão final de carga configurada
} power_info_t;

void power_init(void);
void power_get(power_info_t *out);

// Histórico: uma amostra por minuto, 12 h em PSRAM. "bat?" {"n": pontos} -> "bat" com tudo + "hist".
#include "cJSON.h"
#include "link.h"
bool power_handle(const cJSON *msg, link_via_t via);
