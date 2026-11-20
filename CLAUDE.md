# DashDeck: notas para o Claude Code

Resumo do projeto e do estado do trabalho, para continuar numa sessão nova sem retomar conversas antigas.
A documentação completa está em `docs/` (português europeu: "ecrã", "ficheiro", "por omissão").

## O que é
Tela auxiliar do ClockworkPi uConsole: uma Waveshare **ESP32-C6-Touch-LCD-1.47** (320×172 na horizontal,
toque), ligada só por USB. O ESP é um renderizador fino (LVGL 9.2); quem decide tudo é o daemon
`host/dashd.py` (systemd --user), configurado em `host/config.toml`. Protocolo: uma linha JSON por
mensagem, até 8 KB (descrito no cabeçalho de `firmware/main/ui.c` e em `docs/arquitetura.md`).

- **Menu inicial (boxes):** AIO v2, Meshtastic, GPS, ADS-B, AIS, SDR, Rede, Aplicações, Serviços, Sistema.
- **Dock** (início e Aplicações): Início, Relógio, Captura, Brilho, Som (volume e brilho do uConsole), Bloquear.
- **Firmware atual:** 0.21.0 (gravado em 20/11).

## Onde está cada coisa
- `host/providers.py`: telas com dados (system, gps, adsb, ais, mesh, clock, aio, sdr, net, console).
- `host/dashd.py`: ligação serial, navegação, envio de ícones, fundos, emojis e avisos.
- `host/clockscene.py`: relógio (Open-Meteo, cena da paisagem em 8 bpp, ícones do tempo).
- `host/emojis.py`: emojis a cores no texto (códigos privados U+100000+, imgfont no ESP).
- `host/meshbridge.py`: cliente da API local do MeshDeckWeb (`~/PycharmProjects/MeshDeckWeb`, `--local-api on`).
- `firmware/main/ui.c`: templates, relógio, ecrã apagado, animação de arranque e aviso de mensagem.
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
- `kill -USR1 $(systemctl --user show -p MainPID --value dashd)`: o ESP responde com `stats` no log do dashd (leituras do toque por segundo, tempo dos quadros com o dedo no ecrã e o maior bloqueio do LVGL por uma mensagem, com o tipo).
- `{"t":"bench"}` (com o dashd parado, por um script que use `dashd.App`): rola a tela atual 2 s e devolve quadros, média e pior tempo.
- Referência na 0.21.0: scroll a 45-46 quadros/s (19 ms por quadro), atualizações até 2,4 ms, abrir Aplicações 166 ms.
- Regra: nas funções `update_*` do `ui.c`, usar `set_text`, `set_text_color`, `set_bg_color`, `set_hidden` e `set_img` (só mudam se o valor for outro).

## Cuidados conhecidos
- **USB da AIO v2:** com o interruptor USB da AIO ligado, a ligação ao ESP perde dados (gravação falha com "Packet content transfer stopped", o ESP pode deixar de ser reconhecido com erro -71). Mantê-lo desligado; o box pede confirmação antes de o ligar.
- **Barramento USB partilhado:** o ESP, o RTL-SDR e discos USB estão no mesmo controlador (dwc2). Cópias grandes pioram a ligação.
- **SDR:** só serve um programa de cada vez. O ADS-B e o AIS ligados pelo DashDeck desligam-se ao sair da tela (`keep_running` mantém-nos). A tela SDR mostra quem o usa e tem "Libertar SDR".
- **Memória do ESP:** o LVGL usa o heap do sistema (`CONFIG_LV_USE_CLIB_MALLOC`); 110 KB ficam reservados para o buffer de ecrã (relógio, mapas, arranque) e sobram ~150 KB. Os ícones vão para o cartão SD (sem reenvio entre arranques) com cópia na RAM enquanto sobram 80 KB de heap (ler do cartão no barramento do display tornava swipes e animações lentos) e uma cache de imagens de 32 KB para os outros; 96 lugares, os do dock nunca são substituídos (`iconlost` avisa o host). O fundo do relógio fica na RAM, porque ler do cartão leva 120+ ms por quadro. O aviso de mensagem (46 KB) usa a memória do fundo do relógio quando o relógio está aberto (`bgfreed`). O `hello` traz `largest` e `sd`.
- **Disco do uConsole:** tem pouco espaço livre; por isso o build fica em `/dev/shm`.

## Feito recentemente (19 e 20/11)
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
1. Mais cenas para o relógio, alarme/temporizador, aplicações abertas, rotação pelo menu, histórico de mensagens no MeshDeckWeb.
2. **Atualizar o firmware pelo cartão SD** (por último; o cartão já está montado em `/sd` desde a 0.17.0): copiar o `.bin` para o cartão; o ESP grava-o em `ota_1` ao arrancar. Exige gravar esta funcionalidade uma vez pela USB.
- Ainda por confirmar com uso real: o aviso "Ondas LoRa" com uma mensagem direta de verdade e a alternância das animações de arranque (só se testou com mensagens fictícias e imagens geradas no uConsole).
