#include "imu.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "link.h"
#include "qmi8658.h"
#include "ui.h"

static const char *TAG = "imu";

static qmi8658_dev_t s_dev;
static imu_info_t s_info = {.face = "?"};
static volatile int s_stream_hz;

static const char *dominant_face(float x, float y, float z)
{
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    if (ax > ay && ax > az) return x > 0 ? "+x" : "-x";
    if (ay > az) return y > 0 ? "+y" : "-y";
    return z > 0 ? "+z" : "-z";
}

static void send_face(const char *face)
{
    link_send_event("imu", "face", face, NULL, NULL);
}

// Posição em pé sobre a mesa (medida na placa): normal = +y para cima (ay ≈ +8,9); girado 90° para a
// esquerda (botões à esquerda) = +x para cima (ax ≈ +9,6); girado 90° para a direita (botões à direita)
// = -x para cima (ax ≈ -9,6); deitado de costas = +z (az ≈ +8,6). O Pomodoro usa o giro para a direita.
// "Em pé" exige o ecrã na vertical (|az| pequeno, até ~20°), parado (giroscópio calmo) e 1 g;
// assim pegar no cubo, incliná-lo ou deitá-lo não conta como giro.
enum { ORI_NONE, ORI_UP, ORI_LEFT, ORI_RIGHT };

static int upright_pose(float ax, float ay, float az, float g, float gyro)
{
    if (fabsf(g - 9.81f) > 1.5f || fabsf(az) > 3.3f || gyro > 40) return ORI_NONE;
    if (ay > 8.0f && fabsf(ax) < 2.5f) return ORI_UP;
    if (ax > 8.0f && fabsf(ay) < 2.5f) return ORI_LEFT;
    if (ax < -8.0f && fabsf(ay) < 2.5f) return ORI_RIGHT;
    return ORI_NONE;
}

static void imu_task(void *arg)
{
    int ori = ORI_NONE, ori_cand = ORI_NONE;
    int64_t ori_t = 0;
    const char *stable = NULL, *cand = NULL;
    int64_t cand_t = 0, last_stream = 0, last_shake = 0;
    int shake_hits = 0;
    int64_t last_knock = 0, last_move = 0, quiet_from = 0, last_up = 0;
    int64_t shake_win = 0;
    while (true) {
        qmi8658_data_t d;
        if (qmi8658_read_sensor_data(&s_dev, &d) == ESP_OK) {
            s_info.ax = d.accelX; s_info.ay = d.accelY; s_info.az = d.accelZ;
            s_info.gx = d.gyroX; s_info.gy = d.gyroY; s_info.gz = d.gyroZ;
            int64_t now = esp_timer_get_time() / 1000;
            float g = sqrtf(d.accelX * d.accelX + d.accelY * d.accelY + d.accelZ * d.accelZ);
            // face estável: a mesma durante 400 ms e sem movimento brusco
            const char *f = dominant_face(d.accelX, d.accelY, d.accelZ);
            if (fabsf(g - 9.81f) < 2.5f) {
                if (f != cand) { cand = f; cand_t = now; }
                else if (f != stable && now - cand_t > 400) {
                    stable = f;
                    s_info.face = f;
                    send_face(f);
                    ui_wake();
                }
            }
            // giro para a direita em pé (Pomodoro) e regresso: a nova posição parada durante 800 ms
            float gyro = fabsf(d.gyroX) + fabsf(d.gyroY) + fabsf(d.gyroZ);
            int o = upright_pose(d.accelX, d.accelY, d.accelZ, g, gyro);
            if (o != ori_cand) { ori_cand = o; ori_t = now; }
            // em pé estável com o Pomodoro ou a varanda abertos (por exemplo, abertos sem passar por "em pé"): fecha
            else if (o == ORI_UP && now - ori_t > 800 && ui_rotated_open() && now - last_up > 1500) {
                last_up = now;
                ui_orient(0);
            }
            else if (o != ORI_NONE && o != ori && now - ori_t > 800) {
                // só o giro a partir de em pé abre: à direita o Pomodoro, à esquerda a varanda do Romeu
                if (ori == ORI_UP && (o == ORI_RIGHT || o == ORI_LEFT)) ui_orient(o == ORI_RIGHT ? 1 : 2);
                else if (o == ORI_UP && (ori == ORI_RIGHT || ori == ORI_LEFT)) ui_orient(0);
                static const char *ON[] = {"none", "up", "left", "right"};
                link_send_event("imu", "ori", ON[o], NULL, NULL);   // diagnóstico: a posição que o IMU viu
                ori = o;
            }
            // batida na mesa: um pico curto com o cubo parado antes (a varanda reage)
            float dev = fabsf(g - 9.81f);
            if (dev > 1.2f) last_move = now;
            if (dev > 2.5f && dev < 14 && quiet_from && now - quiet_from > 400 && now - last_knock > 800 && gyro < 80) {
                last_knock = now;
                ui_imu_event(1);
            }
            if (dev < 0.6f && now - last_move > 400) quiet_from = quiet_from ? quiet_from : now;
            else if (dev >= 0.6f) quiet_from = 0;
            // agitar: 4 picos acima de 2,2 g em menos de 800 ms
            if (g > 2.2f * 9.81f) {
                if (now - shake_win > 800) { shake_win = now; shake_hits = 0; }
                if (++shake_hits >= 4 && now - last_shake > 1500) {
                    last_shake = now;
                    shake_hits = 0;
                    ui_wake();
                    link_send_event("imu", "ev", "shake", NULL, NULL);
                    ui_imu_event(2);
                }
            }
            int hz = s_stream_hz;
            if (hz > 0 && now - last_stream >= 1000 / hz) {
                last_stream = now;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "t", "imu");
                cJSON_AddNumberToObject(o, "ax", roundf(d.accelX * 100) / 100);
                cJSON_AddNumberToObject(o, "ay", roundf(d.accelY * 100) / 100);
                cJSON_AddNumberToObject(o, "az", roundf(d.accelZ * 100) / 100);
                cJSON_AddNumberToObject(o, "gx", roundf(d.gyroX * 10) / 10);
                cJSON_AddNumberToObject(o, "gy", roundf(d.gyroY * 10) / 10);
                cJSON_AddNumberToObject(o, "gz", roundf(d.gyroZ * 10) / 10);
                link_send(o);
                cJSON_Delete(o);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));   // 50 Hz
    }
}

void imu_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;
    if (qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_HIGH) != ESP_OK &&
        qmi8658_init(&s_dev, bus, QMI8658_ADDRESS_LOW) != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 não encontrado");
        return;
    }
    qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_250HZ);
    qmi8658_set_gyro_range(&s_dev, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(&s_dev, QMI8658_GYRO_ODR_250HZ);
    qmi8658_set_accel_unit_mps2(&s_dev, true);
    qmi8658_set_gyro_unit_dps(&s_dev, true);
    s_info.ok = true;
    xTaskCreatePinnedToCoreWithCaps(imu_task, "imu", 4096, NULL, 3, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);   // stack na PSRAM: não escreve na flash
}

void imu_get(imu_info_t *out)
{
    *out = s_info;
}

void imu_handle(const cJSON *msg)
{
    const cJSON *st = cJSON_GetObjectItem(msg, "stream");
    if (cJSON_IsNumber(st)) {
        int hz = st->valueint;
        s_stream_hz = hz < 0 ? 0 : hz > 50 ? 50 : hz;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "face?"))) send_face(s_info.face);
}
