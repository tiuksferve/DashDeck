#pragma once
// Botões físicos: BOOT (GPIO0), IO18 (GPIO18, "user") e PWR (pelo AXP2101, ver power.c).
// Cada gesto vai para o host como {"t":"key","k":"boot|user|pwr","ev":"short|long|double"}; o deckd
// decide a ação (configurável no config.toml). Sem host, BOOT curto = voltar.
typedef enum { KEY_BOOT = 0, KEY_USER, KEY_PWR } key_id_t;
typedef enum { KEY_EV_SHORT = 0, KEY_EV_LONG, KEY_EV_DOUBLE } key_ev_t;

void keys_init(void);
void keys_emit(key_id_t k, key_ev_t ev);   // também usado pelo power.c
