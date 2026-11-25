# DashDeck: notas para o Claude Code

Resumo do projeto e do estado do trabalho, para continuar numa sessão nova sem retomar conversas antigas.
A documentação completa está em `docs/` (português europeu: "ecrã", "ficheiro", "por omissão").

## O que é
Tela auxiliar do ClockworkPi uConsole: uma Waveshare **ESP32-C6-Touch-LCD-1.47** (320×172 na horizontal,
toque), ligada só por USB. O ESP é um renderizador fino (LVGL 9.2); quem decide tudo é o daemon
`host/dashd.py` (systemd --user), configurado em `host/config.toml`. Protocolo: uma linha JSON por
mensagem, até 8 KB (descrito no cabeçalho de `firmware/main/ui.c` e em `docs/arquitetura.md`).

- **Menu inicial (boxes):** AIO, Meshtastic, GPS, ADS-B, AIS, Sondas, SDR, Câmara, Stocks, Rede, Partilhas, Aplicações, Serviços, Sistema.
- **Dock** (início e Aplicações): Início, Relógio, Captura, Brilho, Som (volume e brilho do uConsole), Bloquear.
- **Firmware atual:** 0.25.20 (gravado em 25/11).

## Onde está cada coisa
- `host/providers.py`: telas com dados (system, gps, adsb, ais, mesh, clock, aio, sdr, net, console, windows, camera, stocks).
- `host/sonderx.py`: receptor RS41 sem janela (serviço `--user` `dashd-sonde`, ligado pelo dashd); usa o `rs41mod` compilado em `~/PycharmProjects/SondeDeck/decoders`. Teste sem rádio: `sonderx.py --replay voo.jsonl --speed 50` (linhas do `rs41mod --json`; um voo real exporta-se de `api.v2.sondehub.org/sonde/<série>`, mas tem indicativos de quem recebeu: não vai para o repositório).
- **Partilhas** (`SharesProvider`): liga/desliga o Samba (`smbd` + `wsdd`, `systemctl enable/disable --now`); mostra só estado, endereço e aparelhos ligados (`smbstatus -b --json`), nunca as pastas. A configuração do Samba vive fora do repositório (`/etc/samba/smb.conf`). Um provider pode ter `tile()` com campos extra para a box do menu (`dot`, `sub_color`, ...).
- `host/stocks.py`: cotações (Yahoo Finance, `/v8/finance/chart`) e logotipos (companiesmarketcap.com, cache em `~/.cache/dashd/logos`, fora do repositório).
- `host/camclient.py`: cliente do camd do CamDeck (`~/PycharmProjects/CamDeck`, porta 8090, token local em `~/.config/camdeck/local-token`).
- `host/wltop.py`: janelas abertas no uConsole (cliente Wayland em Python puro; listar, mostrar, minimizar, fechar).
- `host/dashd.py`: ligação serial, navegação, envio de ícones, fundos, emojis e avisos.
- `host/clockscene.py`: relógio (Open-Meteo, cena da paisagem em 8 bpp, ícones do tempo).
- **Fontes do ESP:** `firmware/tools/gen_fonts.py` (ASCII, Latin-1 e a lista `EXTRA`, que inclui o €). Um carácter novo tem de entrar também em `_EXTRA` no `dashd.py`, senão o host tira-o do texto. O mesmo script gera `dash_arrows` (só ↑ ↓, DejaVu Sans Bold 22 px, de `/usr/share/fonts`) para as setas das boxes.
- `host/emojis.py`: emojis a cores no texto (códigos privados U+100000+, imgfont no ESP).
- `host/meshbridge.py`: cliente da API local do MeshDeckWeb (`~/PycharmProjects/MeshDeckWeb`, `--local-api on`).
- `firmware/main/ui.c`: templates, relógio, ecrã apagado, animação de arranque e aviso de mensagem.
- `firmware/main/aboutanim.c`: cenas do "Sobre o DashDeck" (toque no nome na tela inicial). Os créditos dizem "por CT7BRA": o indicativo aparece aí por decisão do dono (é a única exceção à regra dos dados pessoais).
- `firmware/main/clockface.c`, `bootanim.c`, `notify.c`: desenho pixel art direto em RGB565 (só contas inteiras por pixel, o C6 não tem FPU).

## Como trabalhar
- **Python:** usar `/usr/bin/python3` (o `python3` do PATH é um venv sem PIL nem pyserial).
- **Reiniciar o daemon:** `systemctl --user restart dashd`. Os logs saem em `systemctl --user status dashd -n 50`.
- **Firmware:** `firmware/flash.sh` (ativa o IDF com `. ~/esp/idf-env.sh`, compila em `/dev/shm/dashdeck-build`, pára e volta a ligar o dashd). Se a gravação normal falhar, repete sozinho no modo seguro (sem stub, 115200).
- **Testar desenhos sem o ESP:** compilar `clockface.c`, `bootanim.c` ou `notify.c` com o gcc do uConsole num pequeno programa que grave PPM, e ver a imagem.
- **Testar telas sem efeitos:** usar uma ponte falsa no `MeshtasticProvider`. Abrir uma conversa marca-a como lida no MeshDeckWeb e um toque numa frase envia uma mensagem real.
- **sudo:** o dashd usa `sudo -n` para `systemctl`, `fuser` e `kill` (serviços e SDR).
- **Documentação:** atualizar `docs/` e o README a cada mudança. Registar a versão do firmware em `docs/pendencias.md` (histórico) e subir `DASHDECK_FW_VERSION` em `firmware/main/ui.h`.
- **Git:** branch `main`, remoto `github.com/tiuksferve/DashDeck` (público, MIT). Commits em inglês. Quem faz o push é o dono. Não pôr dados pessoais nos ficheiros (indicativo, utilizador, número de série da placa).
- **MeshDeckWeb** (outro repositório): documentação em inglês e português, CHANGELOG, suíte completa de testes (`.venv/bin/python -m pytest`), branch a partir da `main`. As novidades ficam opcionais e desligadas por omissão.

## Medir o desempenho
- `kill -USR1 $(systemctl --user show -p MainPID --value dashd)`: o ESP responde com `stats` no log do dashd (leituras do toque por segundo, tempo dos quadros com o dedo no ecrã, o maior bloqueio do LVGL por uma mensagem, com o tipo, e a memória: heap, maior bloco, ícones na RAM/cartão). Os contadores zeram a cada pedido.
- `kill -USR2 <pid do dashd>`: captura do ecrã do ESP em `~/Pictures/dashdeck-<data>.png` (`{"t":"shot"}`, 0.25.14). Os prints de `docs/img/` (apresentação em `docs/presentation.md` e `docs/apresentacao.md`) foram tirados assim; não usar telas com dados pessoais (nós e indicativo do Meshtastic, posição do GPS, redes Wi-Fi, número de série do dongle).
- `{"t":"bench"}` (com o dashd parado, por um script que use `dashd.App`): rola a tela atual 2 s e devolve quadros, média e pior tempo.
- Referência na 0.23.0: scroll a 44-46 quadros/s na tela inicial e nas Aplicações (lista toda: `{"t":"bench","px":3000}`), atualizações até 2,4 ms, abrir Aplicações ~100-160 ms. Memória livre nas Aplicações ~43-52 KB (os ícones ocupam o resto; relógio e aviso libertam-nos com o `make_room`).
- Regra: nas funções `update_*` do `ui.c`, usar `set_text`, `set_text_color`, `set_bg_color`, `set_hidden` e `set_img` (só mudam se o valor for outro).

## Cuidados conhecidos
- **USB da AIO v2:** com o interruptor USB da AIO ligado, a ligação ao ESP perde dados (gravação falha com "Packet content transfer stopped", o ESP pode deixar de ser reconhecido com erro -71). Mantê-lo desligado; o box pede confirmação antes de o ligar.
- **Barramento USB partilhado:** o ESP, o RTL-SDR e discos USB estão no mesmo controlador (dwc2). Cópias grandes pioram a ligação.
- **SDR:** só serve um programa de cada vez. O ADS-B e o AIS ligados pelo DashDeck desligam-se ao sair da tela (`keep_running` mantém-nos). A tela SDR mostra quem o usa e tem "Libertar SDR".
- **Memória do ESP:** o LVGL usa o heap do sistema (`CONFIG_LV_USE_CLIB_MALLOC`); 110 KB ficam reservados para o buffer de ecrã (relógio, mapas, arranque) e sobram ~150 KB. Os ícones vão para o cartão SD (sem reenvio entre arranques) com cópia na RAM para os da tela aberta e do dock (carregados ao construir a tela, libertando os de outras telas, enquanto sobram 20 KB de heap (eram 36 KB até à 0.25.1: depois de algum uso os ícones das Aplicações ficavam no cartão e o scroll bloqueava ~400 ms); ler do cartão no barramento do display tornava swipes e animações lentos) e uma cache de imagens de 32 KB para os outros; 96 lugares, os do dock nunca são substituídos (`iconlost` avisa o host). O fundo do relógio fica na RAM, porque ler do cartão leva 120+ ms por quadro. O aviso de mensagem (46 KB) usa a memória do fundo do relógio quando o relógio está aberto (`bgfreed`). O `hello` traz `largest` e `sd`; o `stats` traz `heap`, `largest`, ícones na RAM e no cartão e emojis. O fundo do mapa é libertado fora dos mapas (0.25.2).
- **Disco do uConsole:** tem pouco espaço livre; por isso o build fica em `/dev/shm`.

## Feito recentemente (19 a 23/11)
- **Stocks** (23/11): box com Siemens, Tesla, SpaceX (SPCX), Apple e NVIDIA; logotipo, cotação em destaque (`value` da box, 0.25.8), variação, seta ↑/↓ (`mark`) e ponto da bolsa aberta/fechada (`dot`, 0.25.9); detalhe em duas páginas: gráfico do dia (plot com `tracks` e `texts` para horas e valores; antes da abertura a sessão é recuada para a dos dados) e informações. Detalhes dinâmicos podem ter páginas: o provider define `detail_pages(kind)` e `render_detail(kind, page)`.
- **Serviços** (21/11): 16 serviços revistos no uConsole, incluindo os do utilizador (`user = true`: dashd com `restart_only`, camd, rpi-connect); botões de arranque no boot; `service_states()` lê todos numa chamada.
- **Monitor de emergências ADS-B** (21/11): cartão no ADS-B (`AdsbProvider.monitor`, `adsb_monitor` em `state.json`) que mantém o readsb ligado; `check_squawk()` no dashd só corre com ele ligado; aviso `notify` `kind:"squawk"` (avião em `notify.c`) e avião no cabeçalho (`hdr`). Testado com readsb simulado e avião fictício (7700); falta um caso real.
- **Scroll** (21/11): inércia mais longa (`scroll_throw` 4), ícones na RAM até sobrarem 20 KB, fundo do mapa libertado fora dos mapas.
- **Câmara** (21/11): box com visor ao vivo (JPEG reduzido no host, `{"t":"jpg"}` descodificado pela ROM do C6 em ~40 ms, fluxo com `jpgack`), Foto e Gravar por cima da imagem, página Galeria (miniaturas do camd; vídeos tocados pelo ffmpeg a partir de uma cópia em `/dev/shm`, para o token não aparecer na linha de comando) e página Estado. ~16 quadros/s quando a câmara os dá. Precisa do camd com o token local e o `?after=n` no `/api/frame.jpg` (alterações no CamDeck, 21/11). Os toques nos botões ainda não foram feitos com o dedo (só por eventos simulados).
- **Rotas** (20/11): ADS-B com origem/destino pelo indicativo (`host/routes.py`: adsbdb.com com companhia, hexdb.io como reserva; cache em `~/.cache/dashd/routes.json`); AIS com destino na lista e no mapa e chegada (ETA) no detalhe. Visto a funcionar com voos reais a 21/11 (o AFR36JZ só existe no adsbdb).
- **Programas abertos pelo DashDeck** (21/11) correm num scope próprio (`spawn_detached`, `systemd-run --user --scope`): antes ficavam no grupo do dashd.service e o systemd fechava-os quando o dashd parava (fechou o PyCharm).
- **Relógio legível** (20/11): `scrim()` (sombra translúcida esbatida, opacidade `SCRIM_A`) atrás dos blocos de texto e `outline_px()` (contorno 2 px + sombra) em `clockface.c`. Desenho ~14 ms por quadro.
- **Abertas** (20/11): box à frente nas Aplicações com os programas abertos (labwc, protocolo wlr-foreign-toplevel); Mostrar, Minimizar, Fechar. Testado com o gestor de ficheiros.
- **Rotação pelo menu** (20/11): Sistema, página DashDeck (Vertical/Horizontal, Inverter), gravada em `~/.config/dashd/state.json`. Em retrato, 43 quadros/s na tela inicial e nas Aplicações.
- **Relógio** (20/11): mostrador **cidade**, neve no inverno na paisagem, toque longo no retro muda a cor, mostrador **temporizador e alarme** (desenho e zonas em `clockface.c`; o estado e o toque vivem no `ClockProvider`, `tick()` a cada segundo no dashd; aviso com o despertador em `notify.c`; bip pelo `pw-play`). As zonas de toque e a repetição ainda não foram usadas com o dedo.
- **AIO v2: perfis de uso** (20/11): box Perfis com Meshtastic, ADS-B, AIS e Poupança (`[[menu.profiles]]`); o ativo fica verde. Testados só a abrir e o "já está ativo": aplicar um perfil mexe no hardware, ainda não foi feito com o utilizador.
- **Rede** (20/11): Wi-Fi, redes guardadas/abertas com um toque, VPN do NetworkManager e Tailscale (nenhuma VPN configurada no uConsole por agora).
- **Som** no dock (20/11): volume (wpctl) e brilho do ecrã do uConsole (backlight 0 a 9).
- Meshtastic: página **Mapa** dos nós (20/11), página **Conversas** (canais e diretas; cada uma abre as suas mensagens; marca como lida só a conversa aberta), **Responder** em cada mensagem, **Mensagem direta** no detalhe do nó, aviso animado **"Ondas LoRa"** para diretas recebidas (o toque abre a conversa).
- **SDR:** monitor (alimentação, dongle, quem usa, frequência, histórico, Libertar).
- **AIO v2:** interruptores com informação ao vivo, box **Energia** e medição do consumo por recurso (`aiov2_ctl --measure`, só a bateria).
- **Arranque:** animação Pixels ou Nascer do sol (aleatória com entropia do ADC), com barra de progresso até o uConsole responder.
- **Relógio:** paisagem, digital retro e previsão por horas (costa com farol; 20/11), swipe troca o mostrador e muda o brilho, dois toques para sair, escurece após 5 min.
- Nome mudado de DashOS para DashDeck (também no MeshDeckWeb, branch `rename-dashdeck`).

## Por fazer (por prioridade sugerida)
1. Pequenas melhorias sugeridas (20/11), por esta ordem: nomes dos portos no AIS (tabela UN/LOCODE pequena: `PT LIS` → Lisboa);
   aviso de bateria fraca do uConsole (15% e 5%);
   resumo do dia no ADS-B/AIS (vistos hoje, o mais distante); filtros (altitude no ADS-B, tipo no AIS); guardar os trajetos do ADS-B.
   Histórico de mensagens no MeshDeckWeb.
2. **Atualizar o firmware pelo cartão SD** (por último; o cartão já está montado em `/sd` desde a 0.17.0): copiar o `.bin` para o cartão; o ESP grava-o em `ota_1` ao arrancar. Exige gravar esta funcionalidade uma vez pela USB.
- Ainda por confirmar com uso real: o aviso "Ondas LoRa" com uma mensagem direta de verdade e a alternância das animações de arranque (só se testou com mensagens fictícias e imagens geradas no uConsole).
