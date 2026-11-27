# DashDeck: tela auxiliar do uConsole

> Também serve o **DashDeck³**, um ecrã AMOLED 480×480 em cubo (ESP32-S3) ligado por Wi-Fi: o mesmo serviço e
> os mesmos módulos, nos dois ecrãs ao mesmo tempo. Ver [docs/deck3/](docs/deck3/README.md).

Apresentação curta com capturas: [English](docs/presentation.md) · [Português](docs/apresentacao.md)

O DashDeck transforma uma **Waveshare ESP32-C6-Touch-LCD-1.47** (320×172 na horizontal, com toque) num
painel de controle e informação do **ClockworkPi uConsole** (CM5, Pi OS com Wayland/labwc e placa
AIO v2 com RTL-SDR, LoRa e GPS). A placa está ligada só por **USB-C**, sem fios nos GPIO do uConsole.

Na tela você pode:
- abrir as aplicações do uConsole;
- ligar e parar serviços;
- ver dados ao vivo: Meshtastic, GPS, ADS-B, AIS e o estado do sistema;
- ligar e desligar o readsb e o AIS-catcher a partir da tela, que partilham o RTL-SDR;
- enviar respostas rápidas pela rede Meshtastic.

```
 uConsole                                                    ESP32-C6 (DashDeck, LVGL 9)
 ┌──────────────────────────────┐   JSON, uma linha/msg   ┌───────────────────────────────┐
 │ dashd (Python, systemd user) │ ◀═════ USB-C (CDC) ════▶│ só desenha telas genéricas    │
 │  config.toml → menus, ações  │                         │ menu · list · kv · log · text │
 │  providers → dados ao vivo   │                         │ plot (mapa, céu do GPS)       │
 └──────┬───────────────────────┘                         │ toque, swipe, botão BOOT      │
        │ HTTP 127.0.0.1:8080                             └───────────────────────────────┘
        ▼
 MeshDeckWeb ──TCP 4403──▶ meshtasticd (rádio LoRa)
```

O ESP **não guarda lógica**: menus, dados e ações ficam no daemon `dashd` do uConsole. Para
mudar o menu basta editar `host/config.toml` e reiniciar o serviço, sem regravar o ESP.

## Arranque rápido

```bash
systemctl --user status dashd       # estado (arranca sozinho com a sessão)
systemctl --user restart dashd      # depois de editar host/config.toml
systemctl --user stop dashd         # libertar a porta /dev/esp-dash
systemctl --user status dashd -n 30 --no-pager   # últimas linhas do log
```

- **Para depurar**, rode em primeiro plano: `systemctl --user stop dashd && /usr/bin/python3 host/dashd.py -v`.
  Use o `/usr/bin/python3`: o `python3` do PATH é um venv que não tem as dependências.
- **Para gravar o firmware**, rode `firmware/flash.sh`. Ele para o dashd, grava e volta a ligá-lo.
- **Para instalar noutro uConsole**, siga [docs/instalacao.md](docs/instalacao.md).

## Na tela

- **Menu inicial em boxes com scroll lateral**: AIO, Meshtastic, GPS, ADS-B, AIS, Sondas, SDR, Câmara, Stocks, Rede, Partilhas, Aplicações, Serviços e Sistema.
- **Barra de cima**: botão ←, título, pontos de página, hora, bateria do uConsole e ícone da ligação USB (verde ou vermelho).
- **Dock** (na tela inicial e em Aplicações): Início, Relógio, Captura, Brilho, Som e Bloquear.
- **Ecrã apagado** após 3 min sem toques na tela inicial; um toque liga-o.
- **Swipe para os lados** nas telas com várias páginas (GPS, ADS-B, AIS, Meshtastic, Rede).
- **Para voltar**: o botão ← no topo, o **botão BOOT** da placa ou o Início no dock.

Os detalhes de cada tela estão em [docs/uso.md](docs/uso.md).

## Estado

| Módulo | O que faz | Estado |
|---|---|---|
| ADS-B (emergências) | Monitor opcional: com ele ligado, aviso animado em qualquer tela quando um avião transmite 7500, 7600 ou 7700, e um avião laranja no cabeçalho | ok |
| ADS-B e AIS (rotas) | Origem, destino e companhia de cada voo (adsbdb.com e hexdb.io, pelo indicativo); destino e chegada prevista de cada navio (do próprio AIS) | ok |
| Aplicações | Os programas de Acessórios do uConsole, lidos dos `.desktop` com o ícone real, mais Terminal, Gqrx, Arquivos e PyCharm; box **Abertas** com os programas abertos (mostrar, minimizar, fechar) | ok |
| Meshtastic | Painel, Nós, Conversas e Mapa dos nós (canais e diretas; cada uma abre as suas mensagens, verdes para RF e lilás para MQTT), com Responder em cada mensagem, Mensagem direta no detalhe do nó e o aviso animado de mensagem direta. As não lidas aparecem como selo vermelho no box. Os dados vêm pela API local do MeshDeckWeb | ok |
| GPS | GPS ao vivo pelo MeshDeckWeb (com o gpsmux, partilhado com o meshtasticd), posição pelo Meshtastic, ou NMEA direto do `/dev/ttyAMA0`. Página com o céu dos satélites | ok |
| ADS-B | Liga e desliga o readsb. Lista de aeronaves, detalhe e mapa com trajetos, zoom e setas sobre o mapa, e seguir uma aeronave | ok |
| AIS | Liga e desliga o AIS-catcher. Lista de navios, detalhe e o mesmo mapa com trajetos e seguir um navio | ok |
| AIO | Interruptores GPS, LoRa, SDR e USB da placa AIO v2 (pelo `aiov2_ctl`), com informação ao vivo de cada um, perfis de uso num toque (Meshtastic, ADS-B, AIS, Poupança), a energia da bateria e a medição do consumo por recurso | ok |
| SDR | Monitor do RTL-SDR: alimentação, dongle, quem o usa, frequência, histórico e "Libertar SDR" | ok |
| Câmara | Visor ao vivo da câmara do CamDeck (XIAO ESP32-S3 Sense), Foto e Gravar por cima da imagem, galeria (fotos e vídeos, que tocam no ecrã), estado e resolução do vídeo e das fotos | ok |
| Sondas | Radiossondas RS41 com o descodificador do SondeDeck: frequências europeias (Lisboa no topo), mapa com o trajeto, rebentamento e pouso previsto, dados, altitude e estatísticas | novo (testado com a reprodução de um voo real; falta um voo ao vivo) |
| Stocks | Cotações (Yahoo Finance) de Siemens, Tesla, SpaceX, Apple e NVIDIA: logotipo, cotação e variação do dia a cores; gráfico do dia e informações em duas páginas | ok |
| Rede | Wi-Fi (estado, sinal, Internet, IP), redes ao alcance (as guardadas ligam-se com um toque) e VPN (NetworkManager e Tailscale) | ok |
| Som | Volume e brilho do ecrã do uConsole, pelo dock | ok |
| Serviços | Ligar, parar e reiniciar unidades systemd, com o grupo exclusivo `sdr` (um só uso do RTL-SDR) | ok |
| Sistema | CPU, temperatura, RAM, disco, bateria e IPs; página DashDeck com o firmware e a rotação do ecrã (vertical, horizontal, inverter) | ok |
| Relógio | Ecrã inteiro em pixel art com 5 mostradores: paisagem que segue o sol, o tempo e as estações, digital retro (cor por toque longo), previsão por horas na costa, cidade à beira-rio, e temporizador e alarme com aviso animado e bip no uConsole; swipe troca o mostrador e muda o brilho | ok |
| Dock | Comandos fixos configuráveis | ok |

O que falta e as limitações conhecidas estão em **[docs/pendencias.md](docs/pendencias.md)**.

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/instalacao.md](docs/instalacao.md) | Instalar do zero: dependências, udev, serviço, ESP-IDF, gravar o firmware e a ponte MeshDeckWeb |
| [docs/uso.md](docs/uso.md) | Navegação, gestos e cada módulo na tela |
| [docs/configuracao.md](docs/configuracao.md) | Referência do `host/config.toml`: tipos de item, providers e dock |
| [docs/arquitetura.md](docs/arquitetura.md) | Como funciona: firmware, daemon, protocolo serial e a ponte Meshtastic |
| [docs/pendencias.md](docs/pendencias.md) | O que falta, ideias e limitações conhecidas |
| [docs/deck3/](docs/deck3/README.md) | O DashDeck³: primeiro arranque, protocolo, arquitetura e a união com o DashDeck |

## Estrutura

```
host/                 lado do uConsole (Python 3.11, /usr/bin/python3)
  dashd.py            serviço: navegação, ações, envio de ícones e mapas; uma sessão por ecrã
  session.py          estado de cada ecrã (sessão)
  transports/         serial_link.py: o DashDeck pela USB
  deck.py             o DashDeck³: WebSocket, servidor (/deck, /fw/, /ctl), botões, bateria, OTA
  deckproviders.py    box Config do DashDeck³; deckctl.py (controlo local) e fakedeck.py (cubo falso)
  paths.py            pastas de estado, cache e registos (~/.config/dashd, ...)
  providers.py        dados ao vivo: system, gps, adsb, ais, mesh, clock, aio, sdr; trajetos e uso do SDR
  clockscene.py       relógio: previsão (Open-Meteo), cenas (lago, costa, cidade; 8 bpp) e ícones do tempo
  meshbridge.py       cliente da API local do MeshDeckWeb
  emojis.py           emojis no texto: separa as sequências e desenha-as a cores para o ESP
  maptiles.py         tiles do mapa (Web Mercator), cache e redução a 16 cores
  icons.py            ícones das apps (tema GTK, emoji) → RGB565A8
  xdgapps.py          lista de aplicações a partir dos .desktop
  config.toml         menus, dock e opções
  dashd.service       unidade systemd --user
firmware/             ESP-IDF 5.5 + LVGL 9.2 (ESP32-C6)
  main/ui.c           renderizador de telas, protocolo (cabeçalho do arquivo)
  main/clockface.c    mostradores do relógio em pixel art
  main/bootanim.c     animação de arranque (Pixels ou Nascer do sol)
  main/link.c         USB-Serial-JTAG, linhas JSON, logs "# ..."
  main/board.c        display JD9853, toque AXS5106L, backlight, rotação
  main/fonts/         fontes Montserrat com acentos (geradas)
  components/         drivers do display e do toque (do demo Waveshare, corrigidos)
  tools/gen_fonts.py  gera main/fonts a partir de tools/fonts/Montserrat-Medium.ttf
  flash.sh            compila e grava (build em /dev/shm)
firmware/deck3-s3/    firmware do DashDeck³ (ESP-IDF 5.5 + LVGL 9.6, ESP32-S3; notas em CLAUDE.md)
setup/                regras udev, ativação do IDF e install.sh (pergunta que aparelhos há)
setup/deck3/          DashDeck³: provision.py (Wi-Fi e token pela USB), mDNS, udev, IDF do S3
docs/                 esta documentação
```

## Licença
MIT (ver [LICENSE](LICENSE)).

## Licenças de terceiros
- Montserrat: SIL Open Font License (`firmware/tools/fonts/OFL.txt`).
- Drivers JD9853 e AXS5106: derivados do demo da Waveshare.
- Tiles do mapa: © Esri (estilos `simples` e `satelite`) e © OpenStreetMap (`osm`). Use-os com moderação; o dashd guarda-os em cache.
