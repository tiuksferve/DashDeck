#pragma once
// Atualização do firmware por Wi-Fi: {"t":"ota","url":"http://<uConsole>:8765/fw/deck3.bin"}.
// Grava na outra partição OTA, envia {"t":"ota","pct":N} e no fim {"t":"ota","ok":true} e reinicia.
#include <stdbool.h>

#include "cJSON.h"

bool ota_handle(const cJSON *msg);
