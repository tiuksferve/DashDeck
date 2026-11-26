# Deck³: notas para o Claude Code

Evolução do DashDeck (`~/PycharmProjects/DashDeck`) para uma Waveshare **ESP32-S3-Touch-AMOLED-2.16**
(cubo, AMOLED 480×480, bateria, som, 2 microfones, IMU, botões BOOT/IO18/PWR), ligada ao uConsole
**por Wi-Fi** (WebSocket + token, descoberta por mDNS). Nome: "Deck³" (o ³ é U+00B3, existe nas fontes).
Estilo visual: o do DashDeck (fundo escuro, cartões) com **ícones e animações em voxels** (`voxel.c`).
Documentação em `docs/` em português europeu ("ecrã", "ficheiro"), como no DashDeck.

## Estado (25/11/2026)
- Preparado antes de a placa chegar. Firmware 0.1.0 **compila mas nunca correu no hardware**.
  Primeiro passo com a placa: `docs/instalacao.md` (dia 1 e lista de verificação).
- deckd testado de ponta a ponta com `host/fakedeck.py`.
- Por fazer: fase 2 (fundos dos mapas, JPEG da câmara, ícones/emojis, dock) e fase 3 (relógio em pixel art).

## Unificação com o DashDeck (decidida a 26/11/2026)
O dono decidiu juntar DashDeck e Deck³ num só projeto (um serviço para os dois ecrãs), na base do repositório
do DashDeck. **Quem conduz é a sessão do DashDeck**; plano completo em `docs/unificacao.md`. Até a união
terminar, esta sessão só mexe no firmware do cubo (`firmware/`), não no `host/`.

## Como trabalhar
- **Firmware:** `firmware/flash.sh build|flash|ota|monitor` (ativa `setup/idf-env.sh`, toolchain Xtensa;
  build em `/dev/shm/deck3-build`). Não usar o `~/esp/idf-env.sh` do DashDeck (é RISC-V/C6).
- **LVGL 9.6** (o `esp_lvgl_adapter` do BSP exige 9.3+); `lv_obj_add_flag`/`remove_flag` estão obsoletas
  mas funcionam (aviso silenciado em `main/CMakeLists.txt`). Bloquear com `bsp_display_lock()` fora das
  callbacks do LVGL; dentro delas (timers, eventos) usar as versões `_nolock`.
- **Testar voxels sem a placa:** compilar `main/voxel.c` com gcc e stubs (`esp_heap_caps.h`, `esp_log.h`,
  cJSON do IDF) num programa que grave PPM/PNG; `docs/preview.png` foi feito assim.
- **Python:** `/usr/bin/python3` (o do PATH é um venv). O deckd só usa a biblioteca padrão; o
  `provision.py` precisa do pyserial.
- **deckd:** `systemctl --user restart deckd`; em primeiro plano `/usr/bin/python3 host/deckd.py -v`.
  Testes: deckd noutra porta (`port = 18765` numa cópia do config) + `host/fakedeck.py --port 18765 --run "..."`.
- **deckd:** o motor (`host/engine.py`) e os providers (`host/providers.py` e módulos de apoio) vieram do
  DashDeck e são do Deck³ (fork, sem dependência). `host/deckd.py` = transporte WebSocket + o que é do cubo;
  `host/deckproviders.py` = providers só do Deck³. Testar telas sem tocar no cubo: `host/deckctl.py tap <id>`,
  `swipe left`, `back`, `home`. Melhorias que sirvam aos dois projetos: levá-las à mão ao DashDeck (com o dono).
- **Memória do firmware:** objetos do LVGL na PSRAM (`main/lv_mem_psram.c`), buffers de desenho na RAM interna
  com DMA (BSP local). Medir com `deckctl.py send '{"t":"stats"}'` + `deckctl.py last stats` (heap/largest).
- **Versão do firmware:** `DECK3_FW_VERSION` em `main/deck.h`.
- **Exemplos da Waveshare** em `~/esp/ref/wsamoled216` (código descarregado: só ler, não compilar lá dentro).

## Estado no hardware (26/11/2026)
- Placa a funcionar com o firmware 0.4.0 (gravado pela USB). Bluetooth **retirado** do firmware (o controlador BLE
  esgotava a RAM interna; código em `main/ble.c`, fora do build). OTA por Wi-Fi funcionou com o 0.1/0.2, mas fica
  para rever (há rollback ativo).
- BSP com correções locais em `firmware/components/waveshare__esp32_s3_touch_amoled_2_16/` (ver `DECK3.md` lá):
  o `bsp_display_lock()` original estava invertido (congelava o ecrã) e a faixa de desenho passou a 20 linhas.
- Animações no estilo do DashDeck em `main/anim.c` (arranque, avisos, "Sobre"), testáveis no uConsole com
  gcc + stubs (ver "Testar voxels sem a placa").
- Cartão SD (SDMMC 1 bit) em `/sdcard/deck3/` (`main/sd.c`, firmware 0.5.0): modelos/cenas de voxels em `vox/`
  (até 24³), imagens PNG/JPG em `icons/` e `scenes/` (no LVGL: `S:icons/x.png`), histórico da bateria
  (`log/bat.bin`) e eventos de fora de casa (`log/pendentes.jsonl`). As escritas passam pela tarefa `sdw`
  (`sd_append`), com stack interno: tarefas com stack na PSRAM não escrevem no cartão diretamente.
  Enviar ficheiros: `host/deckctl.py put <local> <caminho>`; exemplos: `setup/sd_exemplos.py`.
- Abrir a porta USB (monitor) reinicia o chip.

## Cuidados
- **Coexistência com o DashDeck:** o Deck³ não depende dele, mas podem estar no mesmo uConsole: estado e caches
  próprios (`~/.config/deck3`, `~/.cache/deck3`), serviço das sondas próprio (`deck3-sonde`). O SDR, a AIO e o GPS
  são partilhados por natureza (com o ADS-B ligado num, o outro não o tem).
- **USB:** o S3 tem o mesmo VID:PID (303a:1001) que o C6 do DashDeck: a regra udev do DashDeck também
  apanha o Deck³ como `/dev/esp-dash` e o dashd abre-o. Fixar ambas as regras pelo `ATTRS{serial}`.
  O `provision.py` identifica o Deck³ pelo `hello` com `"dev":"deck3"`.
- **AXP2101:** o firmware só lê e ativa interrupções; não mexer nas tensões (alimentam o ecrã e o ESP).
- **AMOLED:** evitar imagens fixas muito tempo (o `idle` escurece aos 60 s e apaga aos 300 s).
- **Git:** repositório local, branch `main`, sem remoto ainda. Commits em inglês; quem faz push é o dono.
  Não pôr dados pessoais nos ficheiros (indicativo, utilizador, número de série da placa: a regra udev
  com o serial fica fora do git ou com o marcador).
