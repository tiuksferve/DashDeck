Cópia local do BSP `waveshare/esp32_s3_touch_amoled_2_16` 2.0.1 (Apache-2.0), que tem prioridade sobre
a versão do `managed_components`. Alterações:
1. `io_config.flags.psram_dma_direct = 1` em `bsp_display_new` (DMA do ecrã direto da PSRAM, poupa
   48 KB de RAM interna por flush).
2. **Bug do BSP 2.0.1:** `bsp_display_lock()` devolvia o `esp_err_t` de `esp_lv_adapter_lock()` como
   `bool`, logo `true` = falhou. Quem fazia `if (!bsp_display_lock(t)) return;` saía com o lock na mão
   (o ecrã congelava) e mexia no LVGL sem lock quando falhava (crashes). Agora devolve `== ESP_OK`, e
   `0` = esperar para sempre.
Ao atualizar o BSP: copiar a nova versão e reaplicar estas alterações (ou confirmar que foram corrigidas).
3. Buffers de desenho do LVGL na RAM interna com DMA (`use_psram = false`, `buffer_height = 12`: 2 x 11,5 KB
   reservados no arranque). Com eles na PSRAM, cada flush pedia um bloco contíguo de RAM interna para a cópia
   DMA, que falhava quando a RAM ficava fragmentada.
