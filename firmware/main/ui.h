#pragma once
#include "cJSON.h"
#include "lvgl.h"

#define DASHDECK_FW_VERSION "0.16.0"

void ui_init(lv_display_t *disp);

// Processa uma mensagem do host (chamado pela tarefa de RX; faz o lock do LVGL).
void ui_handle(const cJSON *msg);
