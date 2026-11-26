# Deck³ (firmware do cubo): notas para o Claude Code

Firmware de uma Waveshare **ESP32-S3-Touch-AMOLED-2.16** (cubo, AMOLED 480×480, bateria, som, 2 microfones,
IMU, botões BOOT/IO18/PWR), ligada ao uConsole **por Wi-Fi** (WebSocket + token, descoberta por mDNS).
Nome: "Deck³" (o ³ é U+00B3, existe nas fontes). Estilo visual: o do DashDeck (fundo escuro, cartões) com
**ícones e animações em voxels** (`main/voxel.c`). Documentação em `docs/deck3/`, em português europeu.

Veio do repositório `~/PycharmProjects/Deck3` (commit `5e81354`), que deixou de receber código a 26/11:
tudo se faz aqui. A união com o DashDeck segue `docs/deck3/unificacao.md`.

## Como trabalhar
- **Compilar e gravar:** `firmware/deck3-s3/flash.sh build|flash|ota|monitor` (ativa `setup/deck3/idf-env.sh`,
  toolchain Xtensa; build em `/dev/shm/deck3-s3-build`). Não usar o `~/esp/idf-env.sh` nem o
  `firmware/flash.sh` do DashDeck (são RISC-V/C6).
- **LVGL 9.6** (o `esp_lvgl_adapter` do BSP exige 9.3+); `lv_obj_add_flag`/`remove_flag` estão obsoletas
  mas funcionam (aviso silenciado em `main/CMakeLists.txt`). Bloquear com `bsp_display_lock()` fora das
  callbacks do LVGL; dentro delas (timers, eventos) usar as versões `_nolock`.
- **Testar voxels e animações sem a placa:** compilar `main/voxel.c` ou `main/anim.c` com gcc e stubs
  (`esp_heap_caps.h`, `esp_log.h`, cJSON do IDF) num programa que grave PPM/PNG; `docs/deck3/preview.png`
  foi feito assim.
- **Memória:** objetos do LVGL na PSRAM (`main/lv_mem_psram.c`), buffers de desenho na RAM interna com DMA
  (BSP local). A RAM interna é muito justa (referência: heap ~47 KB, maior bloco ~27 KB). Medir com
  `deckctl.py send '{"t":"stats"}'` + `deckctl.py last stats`.
- **Versão do firmware:** `DECK3_FW_VERSION` em `main/deck.h`.
- **Host:** até à fase 3 da união, o serviço do cubo é o `deckd` antigo, que corre de
  `~/PycharmProjects/Deck3/host/` (`systemctl --user restart deckd`). O `deckctl.py` e o `fakedeck.py` passam
  para `host/` na fase 3; até lá usam-se de lá.
- **Exemplos da Waveshare** em `~/esp/ref/wsamoled216` (código descarregado: só ler, não compilar lá dentro).

## Estado no hardware (26/11/2026)
- Placa a funcionar (firmware 0.6.0). Bluetooth **retirado** do build (o controlador BLE esgotava a RAM interna;
  o código está em `main/ble.c`). O OTA por Wi-Fi funcionou com o 0.1/0.2, mas fica para rever (há rollback ativo).
- BSP com correções locais em `components/waveshare__esp32_s3_touch_amoled_2_16/` (ver `DECK3.md` lá):
  o `bsp_display_lock()` original estava invertido (congelava o ecrã) e a faixa de desenho passou a 20 linhas.
- Animações no estilo do DashDeck em `main/anim.c` (arranque, avisos, "Sobre").
- Cartão SD (SDMMC 1 bit) em `/sdcard/deck3/` (`main/sd.c`): modelos e cenas de voxels em `vox/` (até 24³),
  imagens PNG/JPG em `icons/` e `scenes/` (no LVGL: `S:icons/x.png`), histórico da bateria (`log/bat.bin`) e
  eventos de fora de casa (`log/pendentes.jsonl`). As escritas passam pela tarefa `sdw` (`sd_append`), com stack
  interno: tarefas com stack na PSRAM não escrevem no cartão diretamente. Enviar ficheiros:
  `deckctl.py put <local> <caminho>`; exemplos: `setup/deck3/sd_exemplos.py`.
- Abrir a porta USB (monitor) reinicia o chip.

## Cuidados
- **USB:** o S3 tem o mesmo VID:PID (303a:1001) que o C6 do DashDeck: a regra udev do DashDeck também apanha
  o Deck³ como `/dev/esp-dash` e o dashd abre-o. As duas regras fixam-se pelo `ATTRS{serial}` (fora do git, ou com
  o marcador). O `setup/deck3/provision.py` identifica o Deck³ pelo `hello` com `"dev":"deck3"`.
- **AXP2101:** o firmware só lê e ativa interrupções; não mexer nas tensões (alimentam o ecrã e o ESP). A corrente
  de carga é escolha do dono (Config › Bateria).
- **AMOLED:** evitar imagens fixas muito tempo (o `idle` escurece aos 60 s e apaga aos 300 s).
- **Dados pessoais:** nada de indicativo, utilizador ou número de série da placa nos ficheiros (a exceção são os
  créditos do "Sobre", por decisão do dono).
