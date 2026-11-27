# Arquitetura

## Hardware (Waveshare ESP32-S3-Touch-AMOLED-2.16)

ESP32-S3R8 (240 MHz, 2 núcleos, FPU), 16 MB de flash, 8 MB de PSRAM octal, Wi-Fi 2,4 GHz (o chip tem BLE, mas o projeto não usa Bluetooth).
Fontes: wiki `docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16`, BSP `waveshare/esp32_s3_touch_amoled_2_16`
2.0.1 e o esquema em `~/esp/ref/wsamoled216/schematic/`.

| Função | Chip | Ligação |
|---|---|---|
| Ecrã 2,16" AMOLED 480×480 | CO5300 | QSPI: CS 12, CLK 38, D0-D3 4-7, RST 39 (brilho por comando 0x51, sem backlight) |
| Toque | CST9220 | I2C partilhado (SDA 15, SCL 14), INT 11, RST 40 |
| PMU e carregador | AXP2101 (0x34) | I2C; botão PWR = PWRON do AXP (sem GPIO; a linha IRQ não chega ao ESP) |
| IMU 6 eixos | QMI8658 (0x6B/0x6A) | I2C; INT1 17, INT2 21 |
| RTC | PCF85063 | I2C; INT 13 (ainda não usado) |
| Altifalante | ES8311 + amplificador | I2S: MCLK 42, BCLK 9, LRCK 45, DOUT 8; PA 46 |
| 2 microfones | ES7210 | I2S: DIN 10 |
| Cartão SD | — | CMD 1, CLK 2, D0 3 (ainda não usado) |
| Botões | — | BOOT = GPIO0, IO18 = GPIO18 (pull-up 10 k), PWR pelo AXP2101 |
| USB-C | USB-Serial-JTAG nativa (303a:1001, como o C6 do DashDeck) | GPIO19/20 |
| Bateria | LiPo 3,7 V (MX1.25) | AXP2101 |

## Firmware (`firmware/`, ESP-IDF 5.5 + LVGL 9.6)

| Ficheiro | Papel |
|---|---|
| `main.c` | NVS → `bsp_display_start` (também inicia o I2C) → ui → link → power → keys → imu → audio → net |
| `link.c` | Fila única de mensagens dos dois transportes (Wi-Fi e USB), tratadas por ordem numa tarefa; logs pela USB como `# ...`; `hello` pela USB sem host |
| `net.c` | Wi-Fi com até 8 redes (escolhe a de melhor sinal), SNTP, descoberta do deckd (fixo → mDNS → último → gateway), WebSocket com token e remontagem de mensagens grandes na PSRAM; configuração `wifi`/`token`/`host`/`cfg?` |
| `ui.c` | Cabeçalho (←, título, páginas, hora, Wi-Fi, ponto da ligação, bateria), templates, confirmações, toasts, avisos em pixel art, progresso da OTA, escurecer/apagar com painel de acordar, ecrã de arranque com o estado da ligação |
| `power.c` | AXP2101: bateria (%, mV), carga, USB, interrupções do PWR (lidas a cada 100 ms). Só lê: não mexe nas tensões |
| `keys.c` | BOOT e IO18: curto, longo (600 ms), duplo (280 ms) |
| `imu.c` | QMI8658 a 50 Hz: face estável para cima, agitar, stream |
| `audio.c` | ES8311/ES7210 pelo `esp_codec_dev` do BSP, 16 kHz estéreo: tons com rampas, nível do microfone |
| `ota.c` | `esp_https_ota` por HTTP para a outra partição |
| `fonts/` | Montserrat 16/20/28/40 com Latin-1 (inclui o ³), geradas por `tools/gen_fonts.py` (o do DashDeck, adaptado) |

**Partições (16 MB):** `nvs`, `otadata`, `phy`, `ota_0` e `ota_1` (4 MB cada; o firmware tem 1,8 MB),
`storage` (spiffs, 7,9 MB).

**Memória:** o LVGL usa o heap do sistema; os buffers grandes (canvas dos ícones, arranque, avisos,
mensagens) vão para a PSRAM. Não há a ginástica de memória do DashDeck (o C6 tinha ~150 KB).

## Daemon (`host/`)

O motor e os providers **vieram do DashDeck** (mesmo autor, MIT) e são agora código do DashDeck³: não é preciso
ter o DashDeck instalado. Se os dois estiverem no mesmo uConsole, cada um guarda o seu estado e as suas caches
(`~/.config/deck3`, `~/.cache/deck3`; o DashDeck usa `dashd`). Os recursos físicos (o RTL-SDR, a AIO, o GPS)
são os mesmos: com o ADS-B ligado num, o outro não tem o SDR.

| Ficheiro | Papel |
|---|---|
| `deckd.py` | `DeckApp(engine.App)`: transporte WebSocket (`WsLink`), hora com a regra POSIX do fuso, área dos plots (444x372), botões físicos (`[keys]`), mensagens do DashDeck³ (bateria, sliders, eventos de fora de casa, SD, OTA), `/ctl` local e `/fw/` |
| `engine.py` | O motor (veio do `dashd.py`): pilha de telas, detalhes dinâmicos, páginas, serviços systemd, lançadores, avisos do Meshtastic, squawks do ADS-B, relógio |
| `providers.py` | Sistema, GPS, ADS-B, AIS, Sondas, Meshtastic, SDR, Câmara, Stocks, Rede, Partilhas, AIO, Consola, Janelas, Relógio |
| `deckproviders.py` | Providers só do DashDeck³ (Config, estado do aparelho) |
| `maptiles.py`, `routes.py`, `meshbridge.py`, `stocks.py`, `camclient.py`, `clockscene.py`, `xdgapps.py`, `icons.py`, `emojis.py`, `wltop.py` | Apoio dos providers (tiles dos mapas, rotas dos voos, API do MeshDeckWeb, cotações, CamDeck, tempo, aplicações, ícones, emojis, janelas) |
| `sonderx.py` + `deck3-sonde.service` | Receptor de radiossondas sem janela (ligado pelo deckd) |
| `ws.py` | WebSocket RFC 6455 mínimo |
| `deckctl.py` | Controlo local: status, notify, toast, beep, send, ota, put/ls/rm (cartão), tap/btn/swipe/back/home (simula o aparelho) |

Dependências: `python3-serial`, `python3-pil`, `python3-gi` (o `setup/install.sh` instala-as). Serviços
opcionais, só para as boxes respetivas: MeshDeckWeb (Meshtastic), readsb (ADS-B), AIS-catcher (AIS),
CamDeck (Câmara), os descodificadores do SondeDeck (Sondas), o aiov2_ctl (AIO).

Opções úteis no `config.toml`: `mark_read = false` no Meshtastic (ler no DashDeck³ não marca a conversa como lida
no MeshDeckWeb) e `tty = ""` no GPS (lê só pelo gpsmux, nunca abre o tty).

Ainda por portar no firmware (fase 2 e 3): fundos dos mapas (`bg`), JPEG da câmara (`jpg`), ícones das
aplicações e emojis (`icon`, `emoji`), dock e o relógio em pixel art (`clock`). O deckd não os envia por agora.
