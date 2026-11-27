#pragma once
// Bluetooth LE (NimBLE). Desligado de origem; o estado fica na NVS ("bt").
// Ligado, o cubo anuncia-se como "DashDeck³ xxxx" e expõe o serviço de bateria (0x180F) e o de
// informação do aparelho: telemóveis e PCs mostram a bateria do DashDeck³. Base para HID/provisioning.
#include <stdbool.h>

#include "cJSON.h"
#include "link.h"

typedef struct {
    bool on;
    bool connected;
    char name[24];
    char addr[18];
    char err[48];
} ble_info_t;

void ble_init(void);                 // liga se estava ligado antes
bool ble_set(bool on);               // false se falhou (ver ble_get().err)
void ble_get(ble_info_t *out);
void ble_battery(int pct);           // atualiza o nível no serviço de bateria
bool ble_handle(const cJSON *msg, link_via_t via);   // "bt" {on} · "bt?"
