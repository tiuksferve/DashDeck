# Arquitetura

## Visão geral

O DashDeck é um **cliente fino**. O ESP32-C6 desenha telas genéricas e devolve os toques. Tudo o que
é lógica fica no uConsole, no daemon `dashd`: menus, dados, ações e navegação.

Vantagens:
- Mudar o menu, acrescentar um módulo ou corrigir um texto é só editar Python ou TOML e reiniciar o serviço.
- O firmware só muda para ganhar um template ou um widget novo.
- O ESP não precisa de Wi-Fi, credenciais nem relógio. A USB dá energia e dados.

**Porquê USB e não Bluetooth:** a tela fica presa ao uConsole, então a USB já dá alimentação, tem
latência baixa e não precisa de emparelhamento. Uma bateria no pino VBAT (o carregador ETA6098 já
está na placa) seria o próximo passo se um dia a tela ficar solta.

```
 dashd.py ─┬─ Link (thread)      serial /dev/esp-dash, reconexão, fila de eventos, fila de acks
           ├─ App (loop)         pilha de navegação, build() de cada tela, envio só do que mudou
           ├─ providers.py       system · gps (NmeaReader) · adsb, ais (+maptiles) · mesh (+meshbridge) · clock (+clockscene) · aio (aiov2_ctl) · sdr
           ├─ icons.py           ícones (tema GTK, ficheiros, emoji) → RGB565A8 32×32
           └─ xdgapps.py         aplicações a partir dos .desktop (cache de 30 s)
```

## Firmware (`firmware/`)

| Arquivo | Papel |
|---|---|
| `main/main.c` | NVS → rotação guardada → `board_init` → `ui_init` → `link_init` |
| `main/board.c` | SPI (80 MHz) e painel JD9853 172×320 (gap 34 px, cores invertidas), I2C e toque AXS5106L, LVGL port (buffer duplo de 40 linhas, DMA, swap de bytes), backlight LEDC, rotação (`lv_display_set_rotation`; o gap passa para Y quando X e Y trocam), botão BOOT (GPIO9) |
| `main/link.c` | Driver USB-Serial-JTAG (RX de 16 KB), uma mensagem JSON por linha (até 8 KB), logs do ESP-IDF reenviados como linhas `# ...`, deteção do host vivo (dados nos últimos 6 s) |
| `main/ui.c` | Templates, toasts, confirmações, páginas e swipe, canvas do plot, fundo 4 ou 8 bpp, ícones na RAM, dock, selos, modo relógio (ecrã inteiro, gestos, hora local) |
| `main/bootanim.c` | Tela de carregamento (Pixels ou Nascer do sol, ao acaso com `esp_random`) com barra de progresso, num canvas próprio na camada de cima. Fica até chegar a primeira tela do host (mínimo 5 s); o buffer de 110 KB é libertado no fim. Sem memória, fica o spinner "Aguardando uConsole" |
| `main/notify.c` | Aviso de mensagem direta ("Ondas LoRa") num canvas 200×110 RGB565A8 (66 KB, transparente) ao centro; o fundo escurecido e a faixa do texto são objetos do LVGL na camada de cima |
| `main/clockface.c` | Mostradores do relógio em pixel art, desenhados direto no buffer RGB565: fontes 5×7 e 3×5, dígitos em blocos, ícone 16×16, chuva, neve e relâmpagos |
| `main/fonts/` | Montserrat Medium 12/14/16/20 com ASCII, Latin-1 e `– — … ← → − ≤ ≥`. O fallback para a Montserrat do LVGL dá os símbolos |
| `components/` | Drivers JD9853 e AXS5106 do demo Waveshare. O `i2c_write` do toque não fazia nada e foi corrigido |

**Pinos da placa** (fiação interna da Waveshare; nada disto liga ao uConsole):

| Função | GPIO |
|---|---|
| LCD JD9853 SPI: SCLK, MOSI, CS, DC, RST, BL | 1, 2, 14, 15, 22, 23 |
| Toque AXS5106L I2C (0x63): SDA, SCL, RST, INT | 18, 19, 20, 21 |
| Cartão SD (SPI partilhado): MISO, CS | 3, 4 |
| Bateria: ADC (VBAT = Vadc × 3) | 0 |
| Botão BOOT | 9 |
| Livres | 7, UART0 16/17 |

**Partições (flash de 8 MB):** `nvs`, `otadata`, `phy`, `ota_0` e `ota_1` (3 MB cada) e `storage`
(littlefs, 1,9 MB). As partições de OTA e de littlefs estão reservadas mas ainda não são usadas.

**Memória:** cerca de 260 KB livres no arranque, organizados assim:
- **Buffer de ecrã reservado (110 KB):** reservado no arranque e reutilizado pela animação de arranque, pelo canvas do relógio e pelo dos mapas. Antes era pedido e devolvido a cada tela, o que fragmentava a memória até não haver um bloco contínuo para o relógio.
- **Ícones no cartão SD:** com cartão, cada ícone é gravado em `/sd/dash/xxxxxxxx.bin` (cabeçalho de imagem do LVGL + RGB565A8) e o LVGL lê-o de `S:dash/...` (sistema de ficheiros stdio). Não gastam RAM e ficam no cartão entre arranques. Sem cartão, ficam na RAM (até 48 de 3 KB).
- **Fundos:** o do relógio (8 bpp, 55 KB) e o dos mapas (4 bpp, ~20 KB) ficam na RAM. Se não houver RAM, vão para `/sd/dash/bg.bin` e são lidos do cartão a cada quadro (lento: 120+ ms; só como reserva).
- **Emojis:** na RAM (até 48 de 630 B), porque o LVGL procura-os a cada letra desenhada.
- **Aviso de mensagem:** canvas de 160×96 RGB565A8 (46 KB). Com o relógio aberto, usa emprestada a memória do fundo do relógio (o relógio fica parado por baixo) e o ESP avisa o host com `bgfreed` para o reenviar depois.
- O `hello` traz `heap`, `largest` (maior bloco contínuo) e `sd` (MB livres no cartão, -1 sem cartão); o dashd regista-os.

**Cartão SD:** no mesmo barramento SPI do display (CS no GPIO4), montado em `/sd` (FAT, nunca formatado pelo DashDeck). Tenta 40 MHz e volta a 20 MHz se o cartão não aguentar.

**Limites:**
- 8 KB por linha JSON;
- 64 itens por tela;
- 8 botões;
- 64 pontos por plot;
- 6 itens no dock;
- 60 linhas no template `log`.

## Protocolo serial

- Porta USB CDC (`/dev/esp-dash`). A velocidade é ignorada: é USB nativa.
- Uma mensagem JSON por linha, em UTF-8.
- O host só deve enviar caracteres que existam na fonte. O `dashd` troca os outros (`lcd_safe`): só o carácter em falta é decomposto ou removido, os acentos do resto ficam.
- **Emojis:** o `dashd` separa cada sequência (símbolo com variação, tom de pele, bandeira, keycap, ZWJ; `host/emojis.py`), desenha-a a cores com 15×14 px a partir da Noto Color Emoji e envia a imagem antes da tela, com confirmação. No texto vai um código privado (U+100000+i). No ESP, uma imgfont do LVGL no fallback das fontes `dash_font_*` desenha esses códigos. Cabem 48; os mais antigos que não estejam na tela são substituídos. O relógio liberta-os junto com os ícones.

### Host → ESP

| `t` | Campos | Efeito |
|---|---|---|
| `screen` | `id`, `tpl`, `title`, `back`, `layout`, `dock`, `pages`, `page` e os dados do template | Desenha uma tela nova |
| `update` | `id` e os dados | Atualiza a tela aberta (só se o `id` for igual). Com o mesmo número de itens, atualiza no lugar; senão refaz o corpo e mantém o scroll |
| `status` | `text` | Texto da barra (hora e bateria) |
| `toast` | `text`, `level` (`ok`, `warn`, `err`, `info`) | Aviso de 2,5 s |
| `bl` | `v` (0 a 100) | Brilho |
| `ping` | — | O ESP responde `pong` (o host envia a cada 2 s) |
| `hello?` | — | Pede um `hello` |
| `rot` | `v` (0 a 3) | Grava a rotação na NVS e reinicia, se mudou |
| `icon` | `id`, `w`, `h` (até 48), `d` (base64 RGB565A8) | Guarda um ícone. O ESP responde `iconack` |
| `bgfreed` (ESP → host) | — | O aviso de mensagem usou a memória do fundo do relógio; o host volta a enviá-lo |
| `emoji` | `cp` (U+100000 + 0..47), `w`, `h` (até 24), `d` (base64 RGB565A8) | Guarda a imagem do emoji que o texto traz como o código privado `cp`. O ESP responde `emojiack` |
| `bg` | `key`, `off`, `d` (base64), na 1.ª parte `w`, `h`, `pal` (16 cores) ou `bpp:8` com `pal` de 256, na última `last` | Fundo do plot (4 bpp) ou do relógio (8 bpp), em pedaços de 2 KB, cada um confirmado com `bgack` |
| `dock` | `items` (`id`, `label`, `icon`, `img`, `color`) | Constrói o dock (só aparece nas telas com `"dock":true`) |
| `idle` | `home` (segundos, 0 = nunca) | Apaga o backlight após N s sem toques na tela `home`. Um painel transparente por cima apanha o toque que acorda |
| `notify` | `from`, `text` | Aviso de mensagem direta por cima de tudo (acende o ecrã). Um toque responde `{"t":"notify","act":"open"}` |
| `time` | `epoch` (segundos, já em hora local) | Acerta o relógio do ESP, que depois conta sozinho. Enviado no `hello` e a cada 10 min |

### Templates (`tpl`)

| `tpl` | Dados |
|---|---|
| `menu` | `items`: `id`, `label`, `sub`, `icon`, `img`, `color`, `badge`. Com `layout:"grid"` são boxes (fileira com scroll lateral na horizontal, grade na vertical); sem ele, cartões |
| `list` | `items`: `id`, `label`, `sub`, `right`, `color`, `bg` (fundo do card), `confirm` (pede Não/Sim antes de enviar o `tap`); `empty` (texto quando a lista está vazia) |
| `kv` | `rows`: `[chave, valor, cor?]`; a cor pode ser `#RRGGBB`, `ok`, `warn`, `err`, `sub` ou `accent` |
| `log` | `lines`, ou `append` no `update` |
| `text` | `text` |
| `plot` | `plot` (canvas) e `rows` (kv ao lado, se couber, ou por baixo) |
| `clock` | `clock`: `face` (0 paisagem, 1 retro), `pal` (cor do LCD), `bl` (brilho ao entrar), `dim` e `dim_bl` (escurece após `dim` s sem toques), `bg` (chave do fundo, prefixo `clk`), `night`, `anim` (`rain`, `storm`, `snow` ou vazio), `wx` (`t`, `max`, `min`, `rain`, `wind`) e `icon` (`pal` até 7 cores, `role` 1 forma / 2 sombra, `d` com 256 dígitos 0–7). Ecrã inteiro, sem barra nem dock |
| todos | `buttons`: `id`, `label`, `color`, `confirm` (texto da confirmação) |

`plot`:
- `w`, `h` em px; `cx`, `cy` e `R` (centro e raio, em px);
- `rings` (raios em px) e `rlabels`;
- `cardinal` (N, S, E, W), `cross` (cruz central), `center` (marca do centro);
- `msg` (aviso no centro) e `bg` (chave do fundo);
- `pts`: `[x, y, cor, rótulo, rumo|null, cheio]`, com x e y em px do canvas (até 80).
- `tracks`: `[cor, largura (1 ou 2), pontos]`, desenhados por baixo dos pontos. Cada ponto são 4
  caracteres base64url (`A-Z a-z 0-9 - _`), 2 por coordenada, com o valor `px + 1024` (aceita
  pontos fora do canvas). O firmware escreve as linhas direto no buffer (Bresenham), sem alocar
  objetos do LVGL. O host gasta no máximo cerca de 2,8 KB de trajetos por tela.
- `ctrl`: `[id, posição, rótulo]`, até 10 botões por cima do canvas. Posições `tl`, `tr`, `bl`,
  `br` (cantos) e `l`, `r`, `t`, `b` (meio das bordas; sem rótulo mostram uma seta). Um toque envia
  `btn` com o `id`. O firmware só refaz os botões quando a lista muda.

### ESP → host

| `t` | Campos | Quando |
|---|---|---|
| `hello` | `fw`, `w`, `h`, `rot`, `heap` | No arranque e a cada 3 s enquanto o host não responde |
| `tap` | `id` (tela), `item` | Toque num item |
| `btn` | `id`, `btn` | Botão (depois da confirmação, se houver) |
| `back` | `id` | Botão ← ou BOOT (no relógio, também dois toques seguidos) |
| `swipe` | `id`, `dir` (`left`, `right`) | Gesto lateral (ignorado nos menus com scroll lateral) |
| `dock` | `item` | Botão do dock |
| `clock` | `face` ou `bl` (texto) | Gesto no relógio, já aplicado pelo ESP; o host guarda a escolha |
| `iconsfreed` | — | O relógio libertou os ícones e o dock; o host volta a enviá-los |
| `pong`, `iconack`, `bgack` | — | Respostas |
| linhas `# ...` | — | Logs do ESP-IDF (o dashd mostra-os com `-v`) |

**Controle de fluxo:** os dados grandes (ícones e mapa) seguem um pedaço de cada vez, e o
próximo só é enviado depois do `ack`. Antes de existir esta confirmação, o buffer USB transbordava
enquanto o ESP desenhava, e o mapa aparecia pela metade. O fundo só é usado se chegar completo e em ordem.

## Daemon (`host/dashd.py`)

- **Navegação:** pilha de nós do `config.toml`. Os providers podem acrescentar telas de detalhe (`on_tap` devolve uma tela).
- **Atualização:** a cada `refresh` s, a tela aberta é reconstruída e só é enviada se o JSON mudou.
- **Ligação:** reabre a porta sozinha, descarta o lixo acumulado e ignora `hello` repetidos (menos de 2 s). Em cada `hello` volta a enviar brilho, estado, dock, ícones e a tela.
- **Rotação:** se o `rot` do `hello` for diferente do `config.toml`, envia `rot` uma vez e o ESP reinicia.
- **Ações:** `launch` usa `gio launch` (`.desktop`) ou um shell com o ambiente Wayland. `service` usa `sudo -n systemctl` numa thread e respeita os grupos exclusivos.

## GPS

- O GPS da AIO v2 está no `/dev/ttyAMA0`, a 9600 baud. Dois leitores na mesma porta repartem os bytes e corrompem os dados.
- **Regra:**
  - se o MeshDeckWeb tem o GPS ao vivo (`GET /api/local/gps` com `state` `fix` ou `searching`), os dados vêm dele (`MeshDeckGps`, com a mesma interface do `NmeaReader`); com o gpsmux do MeshDeckWeb, o `/dev/ttyAMA0` é do gpsmux, que dá ao meshtasticd uma porta virtual (`/run/meshdeck-gps/meshtasticd`) e publica o NMEA em `tcp://127.0.0.1:10110`;
  - senão, se o meshtasticd está ativo e o `config.yaml` tem `GPS: SerialPath`, a porta é dele e a posição vem do nó, pela ponte;
  - senão, o `NmeaReader` abre a porta, valida o checksum e interpreta as frases GGA, RMC, GSA e GSV, e fecha a porta assim que deixar de ser a fonte.
- O gpsd está instalado, mas sem dispositivo (`DEVICES=""`), e não é usado.

## Ponte Meshtastic

- A API TCP 4403 do meshtasticd aceita **um** cliente. Um cliente novo derruba o anterior ("Force close previous TCP connection"), e esse cliente é o MeshDeckWeb.
- Por isso o dashd não se liga ao nó. Pede os dados ao MeshDeckWeb, numa API acrescentada ao projeto dele (`server/local_api.py`):

| Rota | Função |
|---|---|
| `GET /api/local/dash` | Nó local, posição, contagens, não lidas, canais, 100 nós (sem chaves públicas) e as 30 últimas mensagens (com `transport`: `rf`, `mqtt`, `pki` ou `psk`, e `hops`) |
| `POST /api/local/send` | `{"text", "channel"}` ou `{"text", "dest": "!id"}`; até 200 bytes |
| `POST /api/local/read` | `{"key": "ch:0"}` ou `{"all": true}`: marca como lida, também no navegador |

- A API vem **desligada por padrão** (`run_server.py --local-api on|off`, guardado nas definições). Desligada, as rotas dão 404 e o MeshDeckWeb funciona exatamente como antes.
- Ligada, só responde a `127.0.0.1` e `::1`, sem login: qualquer processo do uConsole pode ler e enviar mensagens pelo nó.
- Os testes estão em `tests/test_local_api.py` do MeshDeckWeb.
- O `meshbridge.py` guarda o resumo em cache por 2 s.
- Cor RF/MQTT de uma mensagem: o `transport`; nas cifradas com PKI (que não dizem a via), o `via_mqtt` do último pacote do remetente.
- Os detalhes abertos por um provider (ex.: o seletor de destino) levam `owner`: os toques voltam a esse provider, que pode devolver uma tela, um aviso ou `("back", aviso)` para fechar o detalhe.

## ADS-B, AIS e o SDR

- `TargetMapProvider` (em `providers.py`) é a base comum: card do serviço, lista, detalhe, mapa, trajetos e alvo seguido. `AdsbProvider` e `AisProvider` só dizem de onde vêm os dados, as cores e as linhas do detalhe.
- **Quem usa o SDR:** o dashd procura os RTL-SDR em `/sys/bus/usb/devices` (0bda:2832/2838) e corre `sudo -n fuser /dev/bus/usb/BBB/DDD`. Os processos que não são serviços do grupo (por exemplo `rtl_tcp` ou `intercept.py`) são fechados com `sudo -n kill` depois da confirmação; os serviços do grupo são parados pelo `systemctl`.
- **ADS-B:** o readsb só dá a posição atual (`/run/readsb/aircraft.json`). Uma thread lê-o a cada 2 s e guarda o trajeto de cada aeronave (`Tracker`, `track_minutes`), ignorando movimentos de menos de cerca de 20 m.
- **AIS:** o serviço `ais-catcher` corre com `/etc/AIS-catcher/aiscatcher.json` e serve o visualizador web na porta 8119 (o controlo fica na 8118). O dashd lê `/api/ships.json` e os trajetos em `/api/allpath.json` (cache de 5 s).
- **Seguir:** o centro do mapa fica fixo até o alvo passar 30% da largura ou da altura; aí o mapa recentra nele. Assim o fundo (21 KB) não é reenviado a cada atualização.

## Mapas

- `maptiles.py` usa a projeção Web Mercator, a mesma dos tiles, para os aviões caírem exatamente sobre o mapa.
- O zoom é inteiro, para os tiles não serem reescalados.
- Os tiles são baixados por uma thread, guardados em `~/.cache/dashd/tiles/<hash do servidor>/z/x/y.png` e compostos (base mais camadas transparentes, nos estilos Esri).
- A imagem é reduzida a 16 cores e enviada como 4 bpp, cerca de 21 KB para 308×136.
- São pré-carregados os zooms vizinhos do atual, e não mais, para respeitar as políticas de uso.
