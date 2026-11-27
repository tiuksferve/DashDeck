# DashDeck³ (firmware do cubo): notas para o Claude Code

Firmware de uma Waveshare **ESP32-S3-Touch-AMOLED-2.16** (cubo, AMOLED 480×480, bateria, som, 2 microfones,
IMU, botões BOOT/IO18/PWR), ligada ao uConsole **por Wi-Fi** (WebSocket + token, descoberta por mDNS).
Nome: "DashDeck³" (o ³ é U+00B3, existe nas fontes). Estilo visual: o do DashDeck (fundo escuro, cartões) com
**animações em pixel art de jogo de 8 bits** (`main/anim.c`; as cenas grandes a 240×240 ampliadas 2x). Os voxels
foram retirados a 27/11 (0.12.0): não voltar a usá-los. Documentação em `docs/deck3/`, em português europeu.

Veio do repositório `~/PycharmProjects/Deck3` (commit `5e81354`), que deixou de receber código a 26/11:
tudo se faz aqui. A união com o DashDeck segue `docs/deck3/unificacao.md`.

## Como trabalhar
- **Compilar e gravar:** `firmware/deck3-s3/flash.sh build|flash|ota|monitor` (ativa `setup/deck3/idf-env.sh`,
  toolchain Xtensa; build em `/dev/shm/deck3-s3-build`). Não usar o `~/esp/idf-env.sh` nem o
  `firmware/flash.sh` do DashDeck (são RISC-V/C6).
- **LVGL 9.6** (o `esp_lvgl_adapter` do BSP exige 9.3+); `lv_obj_add_flag`/`remove_flag` estão obsoletas
  mas funcionam (aviso silenciado em `main/CMakeLists.txt`). Bloquear com `bsp_display_lock()` fora das
  callbacks do LVGL; dentro delas (timers, eventos) usar as versões `_nolock`.
- **Ver animações:** capturas do cubo (`deckctl.py shot`) com os comandos de teste `bootanim`, `pomo`,
  `clkface`; ou compilar `main/anim.c` com gcc e stubs (`esp_heap_caps.h`) num programa que grave PNG.
- **Memória:** objetos do LVGL na PSRAM (`main/lv_mem_psram.c`), buffers de desenho na RAM interna com DMA
  (BSP local). A RAM interna é muito justa (referência: heap ~47 KB, maior bloco ~27 KB). Medir com
  `deckctl.py send '{"t":"stats"}'` + `deckctl.py last stats`.
- **Versão do firmware:** `DECK3_FW_VERSION` em `main/deck.h`.
- **Host:** o `dashd` serve o DashDeck e o DashDeck³ (`host/deck.py`; secções `[deckd]`, `[keys]`, `[about]` do
  `host/config.toml`). O serviço instalado (`systemctl --user restart deckd`) ainda corre o da pasta antiga até à
  fase 6 da união. Testar só o cubo noutra porta: `host/deckd.py --deck-port 18765` e
  `host/fakedeck.py --port 18765 --run "tap config; left; back"`. Controlo: `host/deckctl.py [--port N]
  status|tap|swipe|back|home|send|last|put|ls|rm|ota`.
- **Exemplos da Waveshare** em `~/esp/ref/wsamoled216` (código descarregado: só ler, não compilar lá dentro).

## Estado no hardware (26/11/2026)
- Placa a funcionar (firmware 0.6.0). Bluetooth **retirado** do build (o controlador BLE esgotava a RAM interna;
  o código está em `main/ble.c`). O OTA por Wi-Fi funcionou com o 0.1/0.2, mas fica para rever (há rollback ativo).
- BSP com correções locais em `components/waveshare__esp32_s3_touch_amoled_2_16/` (ver `DECK3.md` lá):
  o `bsp_display_lock()` original estava invertido (congelava o ecrã) e a faixa de desenho passou a 20 linhas.
- Animações no estilo do DashDeck em `main/anim.c` (arranque, avisos, "Sobre").
- **UI (0.7.0, 27/11):** cabeçalho com o título numa linha ("Config · Ecrã e som" = título + subtítulo cinzento,
  `set_title`), a hora com o ponto da ligação e, por baixo, Wi-Fi e bateria; pontos das páginas desenhados em baixo.
  Ícones das boxes: o dashd desenha-os (emoji, tema, SVG; `host/icons.py lvgl_bin`) e grava-os no cartão
  (`icons/<id>.bin`, 84×84 ARGB8888); o firmware lê cada um uma vez para a PSRAM (`icon_cached`): ler do cartão a
  cada `update` segurava o LVGL segundos e as mensagens seguintes eram descartadas ("lock do LVGL ocupado").
  Labels do LVGL com `LONG_DOT` só cortam numa linha se tiverem altura fixa (senão quebram). Os sliders não
  propagam o gesto (não mudam de página).
- **Relógio (0.8.2, 27/11):** `tpl "clock"` em ecrã inteiro (`anim.c`, `clock3_t`): 0 paisagem (a cena do lago que o
  dashd desenha a 240×240 e envia por `bg`, com a chuva/neve e o cão do DashDeck por cima), 1 aurora, 2 o cubo,
  3 temporizador e alarme (zonas de toque `anim_clock_zone`). As cenas desenham-se a 240×240 e ampliam-se 2x.
  O fuso para o epoch local calcula-se pela diferença local/UTC (o newlib não tem `tm_gmtoff`).
  O temporizador e o alarme são só do cubo (NVS `tset` `tend` `tleft` `alhm` `alon`; `clock_check` no `tick_1s`,
  toque na coluna do cubo com `notify` `local`); não vão ao dashd. Teste sem dedo: `{"t":"clkzone","v":z}`.
  Avisos (`toast`) e notificações em fila, um de cada vez.
- **Nome (0.10.0):** o sistema do cubo chama-se DashDeck³ (DashDeck elevado ao cubo). Os identificadores
  `deck3` (pasta, mDNS `_deck3`, hostname, `"dev":"deck3"`, `DECK3_*`) ficam: mudá-los quebra a descoberta.
  Arranque (`anim_boot_draw`, 0.11.0; meteoros em 0.12.0): pixel art de 8 bits como a paisagem do DashDeck:
  desenhado a 240×212 em `s_lo` e ampliado 2x (`up2`). Noite com lua e montanhas; as 3 camadas do cubo
  isométrico (`cube8`) caem com ressalto, as letras de DASHDECK caem, o cubo salta para o expoente a encolher
  aos degraus e estoura no ³ (blocos do tamanho das letras, faixas ciano/magenta/âmbar). O nome acaba no
  centro do ecrã. Ver sem reiniciar: `{"t":"bootanim"}`.
- **Pomodoro (0.9.0):** o cubo em pé na mesa e girado 90° para a direita (botões à direita) abre o Pomodoro;
  de volta ao normal, fecha. Eixos medidos: em pé normal ay ≈ +8,9; girado à direita ax ≈ -9,7; girado à
  esquerda ax ≈ +9,6 (livre para outra função); deitado de costas az ≈ +8,6. "Em pé" = |az| < 3,3, 1 g e giroscópio calmo, 800 ms parado (`imu.c`, `ui_orient`); só o
  giro a partir de em pé abre. 25/5/15 min, pausa longa a cada 4 focos; conta com o ecrã fechado
  (`pomo_check`) e avisa no fim de cada fase. Desenho em `anim_pomo_draw` (direito para quem olha) e rodado
  90° no `pomo_draw` (o topo de quem olha é o lado esquerdo do painel). BOOT curto = iniciar/pausar. Teste: `{"t":"pomo","show":true,"z":1,"left":5}`.
- **Mapas:** os tiles chegam por `bg` (`bg.c`, PSRAM) e o `plot.c` desenha-os por baixo. O dashd dá ao cubo uma
  área de 444×320 para o plot (cabe uma fila de botões por baixo).
- **Captura do ecrã:** `host/deckctl.py shot [ficheiro.png]` (sem ficheiro: `~/Pictures/deck3-<data>.png`). Logo
  depois de arrancar pode falhar (o LVGL está ocupado): repetir.
- Cartão SD (SDMMC 1 bit) em `/sdcard/deck3/` (`main/sd.c`): imagens PNG/JPG em `icons/` e `scenes/` (no LVGL: `S:icons/x.png`), histórico da bateria (`log/bat.bin`) e
  eventos de fora de casa (`log/pendentes.jsonl`). As escritas passam pela tarefa `sdw` (`sd_append`), com stack
  interno: tarefas com stack na PSRAM não escrevem no cartão diretamente. Enviar ficheiros:
  `deckctl.py put <local> <caminho>`.
- Abrir a porta USB (monitor) reinicia o chip, mas nem sempre (a 27/11 não reiniciou). Pela USB o cubo também
  aceita mensagens JSON (manda `hello` com `"via":"usb"`); `{"t":"reboot"}` reinicia-o.
- **Wi-Fi "zombie" (27/11):** o cubo ficou associado ao router (sinal verde, −50 dBm) mas sem tráfego: ligações
  ao uConsole e ao gateway davam timeout e não respondia a ping; ficou em "fora de casa" a tentar a cada 4 s.
  Resolveu-se com `{"t":"reboot"}` pela USB. A rever no `net.c`: depois de várias falhas seguidas por timeout
  (incluindo o gateway), desligar e voltar a ligar o Wi-Fi em vez de só repetir o WebSocket.

## Cuidados
- **USB:** o S3 tem o mesmo VID:PID (303a:1001) que o C6 do DashDeck: a regra udev do DashDeck também apanha
  o DashDeck³ como `/dev/esp-dash` e o dashd abre-o. As duas regras fixam-se pelo `ATTRS{serial}` (fora do git, ou com
  o marcador). O `setup/deck3/provision.py` identifica o DashDeck³ pelo `hello` com `"dev":"deck3"`.
- **AXP2101:** o firmware só lê e ativa interrupções; não mexer nas tensões (alimentam o ecrã e o ESP). A corrente
  de carga é escolha do dono (Config › Bateria).
- **AMOLED:** evitar imagens fixas muito tempo (o `idle` escurece aos 60 s e apaga aos 300 s).
- **Dados pessoais:** nada de indicativo, utilizador ou número de série da placa nos ficheiros (a exceção são os
  créditos do "Sobre", por decisão do dono).
