#include "keys.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "ui.h"

#define PIN_BOOT  GPIO_NUM_0
#define PIN_USER  GPIO_NUM_18
#define LONG_MS   600
#define DOUBLE_MS 280

static const char *KNAME[] = {"boot", "user", "pwr"};
static const char *ENAME[] = {"short", "long", "double"};

void keys_emit(key_id_t k, key_ev_t ev)
{
    // com o ecrã a 0%, o botão do brilho não o acende no brilho antigo: o ciclo segue para 25%
    if (!(k == KEY_PWR && ev == KEY_EV_SHORT && ui_bl_off())) ui_wake();
    if (ui_pomo_key(k, ev)) return;   // BOOT curto no Pomodoro: iniciar / pausar
    if (!link_host_alive() && k == KEY_BOOT && ev == KEY_EV_SHORT) {
        ui_local_back();
        return;
    }
    if (!link_host_alive() && k == KEY_PWR && ev == KEY_EV_SHORT) {
        ui_local_bl_cycle();
        return;
    }
    link_send_event("key", "k", KNAME[k], "ev", ENAME[ev]);
}

typedef struct {
    gpio_num_t pin;
    key_id_t id;
    bool down;
    int64_t t_down, t_up;
    bool long_sent, pending;
} btn_t;

static void keys_task(void *arg)
{
    btn_t b[2] = {{.pin = PIN_BOOT, .id = KEY_BOOT}, {.pin = PIN_USER, .id = KEY_USER}};
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        for (int i = 0; i < 2; i++) {
            bool pressed = gpio_get_level(b[i].pin) == 0;
            if (pressed && !b[i].down) {
                b[i].down = true;
                b[i].t_down = now;
                b[i].long_sent = false;
            } else if (!pressed && b[i].down) {
                b[i].down = false;
                if (!b[i].long_sent) {
                    if (b[i].pending && now - b[i].t_up < DOUBLE_MS) {
                        b[i].pending = false;
                        keys_emit(b[i].id, KEY_EV_DOUBLE);
                    } else {
                        b[i].pending = true;
                        b[i].t_up = now;
                    }
                }
            } else if (pressed && !b[i].long_sent && now - b[i].t_down > LONG_MS) {
                b[i].long_sent = true;
                b[i].pending = false;
                keys_emit(b[i].id, KEY_EV_LONG);
            }
            if (b[i].pending && !b[i].down && now - b[i].t_up >= DOUBLE_MS) {
                b[i].pending = false;
                keys_emit(b[i].id, KEY_EV_SHORT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void keys_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BOOT) | (1ULL << PIN_USER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(keys_task, "keys", 2048, NULL, 4, NULL);   // medido: usa ~0,9 KB
}
