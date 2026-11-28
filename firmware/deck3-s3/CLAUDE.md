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
- Placa a funcionar (firmware 0.6.0). **Sem Bluetooth:** o projeto não o usa (código removido a 27/11; já estava
  fora do build, porque o controlador BLE esgotava a RAM interna). Não voltar a ligar `CONFIG_BT_ENABLED`. O OTA por Wi-Fi funcionou com o 0.1/0.2, mas fica para rever (há rollback ativo).
- BSP com correções locais em `components/waveshare__esp32_s3_touch_amoled_2_16/` (ver `DECK3.md` lá):
  o `bsp_display_lock()` original estava invertido (congelava o ecrã) e a faixa de desenho passou a 20 linhas.
- Animações no estilo do DashDeck em `main/anim.c` (arranque, avisos, "Sobre", relógio, Pomodoro).
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
- **Desempenho gráfico (0.13.0, 27/11):** medido com `{"t":"perf"}` (responde `fps`, `refr_ms`, `draw_ms`,
  `wait_ms` desde o pedido anterior; o 1.º pedido só começa a contar). O LVGL leva ~45 ms a mandar um ecrã
  inteiro ao painel (CPU, não o SPI: a 80 MHz ficava igual; `wait_ms` ≈ 0) e o refresco não é paralelo ao
  desenho. Por isso: (1) as telas inteiras (arranque, relógio, Pomodoro, "Sobre") são canvas **RGB565** — o
  `anim.c` escreve em RGB565 nos buffers registados com `anim_out16()` (o resto continua ARGB8888: `s_lo`,
  avisos, ícones); (2) o arranque, o relógio e o "Sobre" desenham o quadro seguinte numa tarefa à parte
  (`fr_task`, duplo buffer `fr_t`, `fr_step`/`fr_submit`) enquanto o LVGL manda o anterior; o `anim.c` tem um
  trinco (`anim_lock`/`ANIM(...)`) porque o estado é partilhado com a tarefa do LVGL; (3) o "Sobre" e o Pomodoro
  estão na camada de topo e a tela ativa passa a uma vazia enquanto estão abertos (`screen_cover`): o LVGL
  redesenhava sempre a tela por baixo. As boxes já não têm animação de entrada (pesava). Resultado: "Sobre"
  de 5,6 para ~16 qps; relógio de ~10 para 17–20. Faixas de 24 linhas davam +2 qps mas deixavam 11 KB de RAM
  interna com o Wi-Fi: ficam 12. `{"t":"dbg"}` (sem o lock do LVGL) diz o estado dos quadros e quem tem o trinco.
  O brilho só se muda na tarefa do LVGL (`bl_set` → `bl_apply_cb`): de outra tarefa, a meio de um envio ao
  painel, o driver SPI dava "Cannot acquire bus when a polling transaction is in progress".
- **"Sobre" (0.13.0):** hiperespaço do DashDeck (`aboutanim.c`) portado para 480×480 1:1 (`anim_about_*`): salto,
  planeta com anéis, lua em órbita, nave, nome DASHDECK³ em pontilhado; créditos como no DashDeck (por CT7BRA,
  endereço do projeto, versões do firmware e do host, linhas do `[about]`) numa faixa em baixo, depois do
  salto, 7 s. Um toque fecha. Desde 0.13.3 alterna com a aurora boreal do DashDeck, recomposta para o ecrã quadrado
  (`about_aurora`, 240×240 ampliada 2x): céu alto com as cortinas a subir da serra até ao nome, picos com neve,
  cabana na colina e o lago no terço de baixo, onde ficam os créditos. **O ecrã é 1:1: cenas do C6 (largas) não se
  portam tal e qual, recompõem-se para o quadrado.** O `s_lo` é partilhado (arranque, relógio, "Sobre"): a serra da aurora
  inicializa-se com marca própria em `lo_ensure()`.
- **Avisos (0.13.0):** sprites como os do DashDeck (`notify.c`), em blocos de 4 px montados numa grelha
  (`sg_mail`, `sg_clock`, `sg_plane`): envelope verde com aba clara e selo, despertador, avião visto de cima.
  Os blocos voam para o lugar; depois um brilho com a forma do sprite (`s_sglow`) pulsa por trás.
- **Varanda do Romeu (0.13.0):** o cubo em pé girado 90° à esquerda (ax ≈ +9,6) abre a varanda (`varanda.c`,
  `var_show` no ui.c); de volta ao normal, fecha. Cena pintada 480×480 com 256 cores e o Romeu em sprites, que o
  dashd converte de `host/assets/varanda` (`host/varanda.py`) e envia para o cartão (`varanda/`, ~540 KB, uma vez
  por versão: marca `v_<versão>.ok`). Dia, entardecer, noite e chuva são trocas de paleta (o fundo e os sprites);
  nuvens, chuva, estrelas, aviões e barcos só no céu e no mar (máscara dos azuis da paleta). Desenho direito para
  quem olha e rodado no `var_draw_fn` (o topo de quem olha é o lado direito do painel); toque: (y, W-1-x).
  Comportamento e desenho na tarefa de quadros; os eventos entram por uma fila (`varanda_event`): toque, BOOT
  (festas), batida na mesa e agitar (`imu.c`, `ui_imu_event`), palmas e barulho (nível local do microfone,
  `audio_mic_local`), carregador, DMs (o `notify` sem `kind` vira o pombo), dados do dashd (`{"t":"varanda"}`).
  Sem tarefas nem necessidades. Testes: `{"t":"varanda","show":true,"mode":"day","ev":"knock"}`. Falta: voz,
  marcos com o tempo, afinar os limiares do microfone e o sentido da inclinação no cubo real.
- **Mapas:** os tiles chegam por `bg` (`bg.c`, PSRAM) e o `plot.c` desenha-os por baixo. O dashd dá ao cubo uma
  área de 444×320 para o plot (cabe uma fila de botões por baixo).
- **Câmara (box do CamDeck):** quadros `jpg` desenhados no canvas `live` do `plot.c` (buffer próprio, que fica entre
  `update`s) com o TJpgDec **do LVGL** (`LV_USE_TJPGD`): tem os mesmos nomes do da ROM e é ele que o linker usa, por isso
  o header é `src/libs/tjpgd/tjpgd.h` (sem redução de escala; entrega B, G, R). ~70 ms por quadro de 444×250.
  Atenção: `/dev/ttyACM0` costuma ser a XIAO da câmara (o cubo é `/dev/deck3`); um esptool na porta errada deixa-a
  sem resposta até `systemctl --user restart camd`.
- **Relógio (0.13.4):** o céu das faces segue a hora e o tempo reais (`clock.sky` do dashd). Aurora: a cena do
  "Sobre" (`about_aurora(t, &sky)`) com céu de dia/crepúsculo/noite, sol e lua no arco, nuvens e chuva/neve; de dia
  não há aurora, ao crepúsculo e com nuvens fica mais fraca. As montanhas, o prado, os pinheiros e a casa são os da
  paisagem do lago (as mesmas fórmulas e cores de `clockscene.render_scene`), para as duas cenas combinarem. Paisagem e aurora: scrim redondo (`scrim_lo`) atrás da
  hora e da previsão, e as nuvens ficam por baixo da zona da hora (no dashd, `render_scene` com w == h). Colorida:
  fundo com o céu de agora (azul fundo de dia, brilho ao crepúsculo, estrelas de noite) e a bateria numa pilha.
- **Emojis no texto:** como no C6 (imgfont no fallback das `deck_font_*`, imagens de 18 px na PSRAM). O registo de que
  emojis cada ecrã tem é por sessão no dashd (`Session.emoji`): os códigos privados do C6 não valem no cubo.
- **Teclado (`input`):** `lv_keyboard` com mapas PT próprios; o `lv_keyboard_def_event_cb` é trocado por `input_kb`
  (tecla "áé" e ✓). Meshtastic: no cubo, "Escrever para" abre o teclado; o C6 fica com as respostas rápidas.
- **OTA e RAM interna:** a tarefa do OTA precisa de 8 KB seguidos de RAM interna (escreve na flash, não pode ter a stack
  na PSRAM). Com pouca RAM livre o OTA falha e o cubo responde `{"t":"ota","err":...}`; aí, gravar pela USB.
- **Captura do ecrã:** `host/deckctl.py shot [ficheiro.png]` (sem ficheiro: `~/Pictures/deck3-<data>.png`). Logo
  depois de arrancar pode falhar (o LVGL está ocupado): repetir.
- Cartão SD (SDMMC 1 bit) em `/sdcard/deck3/` (`main/sd.c`): imagens PNG/JPG em `icons/` e `scenes/` (no LVGL: `S:icons/x.png`), histórico da bateria (`log/bat.bin`) e
  eventos de fora de casa (`log/pendentes.jsonl`). As escritas passam pela tarefa `sdw` (`sd_append`), com stack
  interno: tarefas com stack na PSRAM não escrevem no cartão diretamente. Enviar ficheiros:
  `deckctl.py put <local> <caminho>`.
- **Gravar pela USB quando o `flash.sh` para** ("Connecting..." infinito ou "Packet content transfer stopped"
  depois do stub; aconteceu a 27/11 depois de o cubo ser desligado e religado): gravar sem o stub, a partir da
  pasta do build: `python -m esptool --chip esp32s3 -p /dev/ttyACM0 --no-stub write_flash @flash_args`
  (mais lento, ~2 min). A OTA (`flash.sh ota`) não respondeu nesse dia.
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
