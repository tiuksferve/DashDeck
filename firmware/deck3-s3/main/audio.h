#pragma once
// Som: altifalante (ES8311 + amplificador no GPIO46) e os dois microfones (ES7210), por I2S a 16 kHz.
//  {"t":"beep","f":880,"ms":120,"vol":60}           tom simples
//  {"t":"tones","seq":[[880,90],[0,40],[1320,120]]}  sequência (f=0 é pausa): avisos e alarmes
//  {"t":"vol","v":0..100}                            volume do altifalante
//  {"t":"mic","hz":10}  (0 = parar)                   envia {"t":"mic","db":-35.2,"peak":-20.1}
#include <stdbool.h>

#include "cJSON.h"

void audio_init(void);
bool audio_handle(const cJSON *msg);   // true se a mensagem era de som
void audio_tone(int freq, int ms, int vol);
int audio_get_vol(void);
void audio_mic_local(bool on);             // ler o microfone só para o cubo (varanda), sem enviar ao host
float audio_mic_db(void);                  // último nível (dBFS); -120 sem leitura
