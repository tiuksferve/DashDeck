#pragma once
// IMU QMI8658 (I2C 0x6B/0x6A): face do cubo virada para cima, agitar, inclinação.
// Eventos para o host: {"t":"imu","face":"+z"} quando a face estável muda (os eixos são os do chip;
// o mapeamento para "ecrã para cima" confirma-se com a placa na mão), {"t":"imu","ev":"shake"}.
// {"t":"imu","stream":hz} (0 = parar) envia {"t":"imu","ax","ay","az","gx","gy","gz"} ao vivo.
#include <stdbool.h>

#include "cJSON.h"

typedef struct {
    bool ok;
    float ax, ay, az;   // m/s²
    float gx, gy, gz;   // °/s
    const char *face;   // "+x" "-x" "+y" "-y" "+z" "-z"
} imu_info_t;

void imu_init(void);
void imu_get(imu_info_t *out);
void imu_handle(const cJSON *msg);
